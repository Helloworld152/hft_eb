#include "framework.h"
#include "order_manager.h"
#include "symbol_manager.h"
#include <Python.h>
#include <vector>
#include <sstream>
#include <mutex>
#include <algorithm>

namespace {
std::once_flag g_py_init_flag;

void ensure_python_initialized() {
    std::call_once(g_py_init_flag, []() {
        if (Py_IsInitialized()) {
            LOG_INFO("[PyStrategy] Reusing existing Python runtime.");
            return;
        }
        Py_Initialize();
        PyEval_InitThreads();
        PyEval_SaveThread();
        LOG_INFO("[PyStrategy] Python runtime initialized.");
    });
}

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

void dict_set_double(PyObject* dict, const char* key, double v) {
    PyObject* val = PyFloat_FromDouble(v);
    PyDict_SetItemString(dict, key, val);
    Py_DECREF(val);
}

void dict_set_long(PyObject* dict, const char* key, long long v) {
    PyObject* val = PyLong_FromLongLong(v);
    PyDict_SetItemString(dict, key, val);
    Py_DECREF(val);
}

void dict_set_str(PyObject* dict, const char* key, const char* v) {
    PyObject* val = PyUnicode_FromString(v ? v : "");
    PyDict_SetItemString(dict, key, val);
    Py_DECREF(val);
}

PyObject* safe_get_callable(PyObject* obj, const char* name) {
    PyErr_Clear();
    PyObject* fn = PyObject_GetAttrString(obj, name);
    if (fn && !PyCallable_Check(fn)) {
        Py_DECREF(fn);
        fn = nullptr;
    }
    PyErr_Clear();
    return fn;
}

PyObject* first_available_callable(PyObject* obj, const std::vector<const char*>& names) {
    for (const char* name : names) {
        if (PyObject* fn = safe_get_callable(obj, name)) {
            return fn;
        }
    }
    return nullptr;
}
}

class PyStrategyModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config, ITimerService* timer_svc = nullptr) override {
        (void)timer_svc;
        bus_ = bus;
        ensure_python_initialized();

        py_module_ = get_cfg(config, "py_module", "py_tools.strategies.sample_strategy");
        py_class_ = get_cfg(config, "py_class", "SampleStrategy");
        py_path_ = get_cfg(config, "py_path", "");
        default_account_ = get_cfg(config, "default_account", "");
        error_policy_ = get_cfg(config, "error_policy", "disable");
        sample_every_ = std::max(1, std::stoi(get_cfg(config, "sample_every", "1")));

        if (config.find("symbol_filter") != config.end()) {
            auto symbols = split_csv(config.at("symbol_filter"));
            for (const auto& sym : symbols) {
                symbol_filter_.insert(SymbolManager::instance().get_id(sym.c_str()));
            }
        }

        if (!init_python_strategy(config)) {
            enabled_ = false;
            LOG_ERROR("[PyStrategy] Failed to initialize python strategy. Disabled.");
        }

        bus_->subscribe(EVENT_MARKET_DATA,
                        StaticDelegate<void(void*)>::bind<PyStrategyModule, &PyStrategyModule::on_tick_event>(this));

        if (py_on_kline_) {
            bus_->subscribe(EVENT_KLINE,
                            StaticDelegate<void(void*)>::bind<PyStrategyModule, &PyStrategyModule::on_kline_event>(this));
        }
        if (py_on_order_) {
            bus_->subscribe(EVENT_RTN_ORDER,
                            StaticDelegate<void(void*)>::bind<PyStrategyModule, &PyStrategyModule::on_order_event>(this));
        }
        if (py_on_trade_) {
            bus_->subscribe(EVENT_RTN_TRADE,
                            StaticDelegate<void(void*)>::bind<PyStrategyModule, &PyStrategyModule::on_trade_event>(this));
        }

        LOG_INFO("[PyStrategy] Initialized. module={} class={} on_tick={} on_kline={} on_order={} on_trade={}",
                 py_module_,
                 py_class_,
                 py_on_tick_ ? "yes" : "no",
                 py_on_kline_ ? "yes" : "no",
                 py_on_order_ ? "yes" : "no",
                 py_on_trade_ ? "yes" : "no");
    }

    void start() override {
        call_simple_hook(py_on_start_);
    }

    void stop() override {
        call_simple_hook(py_on_stop_);
        PyGILState_STATE gstate = PyGILState_Ensure();
        Py_XDECREF(py_on_init_);
        Py_XDECREF(py_on_start_);
        Py_XDECREF(py_on_tick_);
        Py_XDECREF(py_on_kline_);
        Py_XDECREF(py_on_order_);
        Py_XDECREF(py_on_trade_);
        Py_XDECREF(py_on_stop_);
        Py_XDECREF(py_instance_);
        Py_XDECREF(py_send_order_);
        Py_XDECREF(py_cancel_order_);
        PyGILState_Release(gstate);
    }

private:
    void on_tick_event(void* d) {
        on_tick(static_cast<TickRecord*>(d));
    }

    void on_kline_event(void* d) {
        on_kline(static_cast<KlineRecord*>(d));
    }

    void on_order_event(void* d) {
        on_order(static_cast<OrderRtn*>(d));
    }

    void on_trade_event(void* d) {
        on_trade(static_cast<TradeRtn*>(d));
    }

    static PyObject* py_send_order(PyObject* self, PyObject* args, PyObject* kwargs) {
        PyStrategyModule* mod = static_cast<PyStrategyModule*>(PyCapsule_GetPointer(self, "PyStrategyModule"));
        if (!mod || !mod->bus_) {
            Py_RETURN_NONE;
        }

        const char* symbol = nullptr;
        const char* direction = nullptr;
        const char* offset = nullptr;
        double price = 0.0;
        int volume = 0;
        const char* account_id = nullptr;

        static const char* kwlist[] = {"symbol", "direction", "offset", "price", "volume", "account_id", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "sssdi|s", const_cast<char**>(kwlist),
                                         &symbol, &direction, &offset, &price, &volume, &account_id)) {
            PyErr_Clear();
            Py_RETURN_NONE;
        }

        OrderReq req{};
        strncpy(req.symbol, symbol, sizeof(req.symbol) - 1);
        req.symbol_id = SymbolManager::instance().get_id(req.symbol);
        req.direction = direction[0];
        req.offset_flag = offset[0];
        req.price = price;
        req.volume = volume;

        if (account_id && account_id[0] != '\0') {
            strncpy(req.account_id, account_id, sizeof(req.account_id) - 1);
        } else if (!mod->default_account_.empty()) {
            strncpy(req.account_id, mod->default_account_.c_str(), sizeof(req.account_id) - 1);
        } else {
            strncpy(req.account_id, "SIM", sizeof(req.account_id) - 1);
        }

        req.client_id = OrderIDGenerator::instance().next_id();
        mod->bus_->publish(EVENT_ORDER_REQ, &req);
        return Py_BuildValue("K", static_cast<unsigned long long>(req.client_id));
    }

    static PyObject* py_cancel_order(PyObject* self, PyObject* args, PyObject* kwargs) {
        PyStrategyModule* mod = static_cast<PyStrategyModule*>(PyCapsule_GetPointer(self, "PyStrategyModule"));
        if (!mod || !mod->bus_) Py_RETURN_NONE;

        unsigned long long client_id = 0;
        const char* symbol = nullptr;
        const char* account_id = nullptr;
        static const char* kwlist[] = {"client_id", "symbol", "account_id", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Ks|s", const_cast<char**>(kwlist),
                                         &client_id, &symbol, &account_id)) {
            PyErr_Clear();
            Py_RETURN_NONE;
        }

        CancelReq req{};
        req.client_id = static_cast<uint64_t>(client_id);
        std::strncpy(req.symbol, symbol, sizeof(req.symbol) - 1);
        if (account_id && account_id[0] != '\0')
            std::strncpy(req.account_id, account_id, sizeof(req.account_id) - 1);
        else if (!mod->default_account_.empty())
            std::strncpy(req.account_id, mod->default_account_.c_str(), sizeof(req.account_id) - 1);
        else
            std::strncpy(req.account_id, "SIM", sizeof(req.account_id) - 1);

        mod->bus_->publish(EVENT_CANCEL_REQ, &req);
        Py_RETURN_NONE;
    }

    bool init_python_strategy(const ConfigMap& config) {
        PyGILState_STATE gstate = PyGILState_Ensure();

        if (!py_path_.empty()) {
            PyObject* sys_path = PySys_GetObject("path");
            PyObject* p = PyUnicode_FromString(py_path_.c_str());
            PyList_Append(sys_path, p);
            Py_DECREF(p);
        }

        PyObject* module = PyImport_ImportModule(py_module_.c_str());
        if (!module) {
            PyErr_Print();
            PyGILState_Release(gstate);
            return false;
        }

        PyObject* cls = PyObject_GetAttrString(module, py_class_.c_str());
        if (!cls || !PyCallable_Check(cls)) {
            PyErr_Print();
            Py_XDECREF(cls);
            Py_DECREF(module);
            PyGILState_Release(gstate);
            return false;
        }
        if (!PyType_Check(cls)) {
            LOG_ERROR("[PyStrategy] py_class must be a class (type), not a plain function.");
            Py_DECREF(cls);
            Py_DECREF(module);
            PyGILState_Release(gstate);
            return false;
        }

        PyObject* config_dict = PyDict_New();
        for (const auto& kv : config) {
            PyObject* v = PyUnicode_FromString(kv.second.c_str());
            PyDict_SetItemString(config_dict, kv.first.c_str(), v);
            Py_DECREF(v);
        }

        PyObject* capsule = PyCapsule_New(this, "PyStrategyModule", nullptr);
        static PyMethodDef send_def = {
            "send_order",
            (PyCFunction)py_send_order,
            METH_VARARGS | METH_KEYWORDS,
            "send order to engine"
        };
        py_send_order_ = PyCFunction_NewEx(&send_def, capsule, nullptr);

        static PyMethodDef cancel_def = {
            "cancel_order",
            (PyCFunction)py_cancel_order,
            METH_VARARGS | METH_KEYWORDS,
            "cancel order by sys_id"
        };
        py_cancel_order_ = PyCFunction_NewEx(&cancel_def, capsule, nullptr);
        Py_DECREF(capsule);

        bool ctor_with_args = true;
        py_instance_ = PyObject_CallFunctionObjArgs(cls, config_dict, nullptr);

        if (!py_instance_) {
            PyErr_Clear();
            ctor_with_args = false;
            py_instance_ = PyObject_CallObject(cls, nullptr);
        }

        if (!py_instance_) {
            PyErr_Print();
            Py_DECREF(config_dict);
            Py_DECREF(cls);
            PyGILState_Release(gstate);
            return false;
        }

        PyObject_SetAttrString(py_instance_, "_send_order", py_send_order_);
        PyObject_SetAttrString(py_instance_, "_cancel_order", py_cancel_order_);

        if (!ctor_with_args && PyObject_HasAttrString(py_instance_, "init")) {
            PyObject* init_fn = PyObject_GetAttrString(py_instance_, "init");
            if (init_fn && PyCallable_Check(init_fn)) {
                PyObject* args = PyTuple_Pack(1, config_dict);
                PyObject* ret = PyObject_CallObject(init_fn, args);
                Py_XDECREF(ret);
                Py_DECREF(args);
            }
            Py_XDECREF(init_fn);
            if (PyErr_Occurred()) {
                PyErr_Print();
            }
        }

        Py_DECREF(config_dict);

        py_on_init_ = first_available_callable(py_instance_, {"handle_init", "on_init"});
        py_on_start_ = first_available_callable(py_instance_, {"handle_start", "on_start"});
        py_on_tick_ = first_available_callable(py_instance_, {"handle_tick", "on_tick"});
        py_on_kline_ = first_available_callable(py_instance_, {"handle_bar", "on_kline"});
        py_on_order_ = first_available_callable(py_instance_, {"handle_order", "on_order"});
        py_on_trade_ = first_available_callable(py_instance_, {"handle_trade", "on_trade"});
        py_on_stop_ = first_available_callable(py_instance_, {"handle_stop", "on_stop"});

        Py_DECREF(cls);
        Py_DECREF(module);

        call_simple_hook(py_on_init_);

        PyGILState_Release(gstate);
        if (py_on_tick_ == nullptr && py_on_kline_ == nullptr) {
            LOG_ERROR("[PyStrategy] Strategy must define on_tick and/or on_kline.");
            return false;
        }
        return true;
    }

    void on_tick(TickRecord* tick) {
        if (!enabled_ || !py_on_tick_) return;

        if (!symbol_filter_.empty() && symbol_filter_.find(tick->symbol_id) == symbol_filter_.end()) {
            return;
        }

        tick_count_++;
        if ((tick_count_ % sample_every_) != 0) return;

        PyGILState_STATE gstate = PyGILState_Ensure();

        PyObject* d = PyDict_New();
        dict_set_str(d, "symbol", tick->symbol);
        dict_set_long(d, "symbol_id", static_cast<long long>(tick->symbol_id));
        dict_set_double(d, "last_price", tick->last_price);
        dict_set_long(d, "volume", tick->volume);
        dict_set_double(d, "turnover", tick->turnover);
        dict_set_double(d, "open_interest", tick->open_interest);
        dict_set_long(d, "trading_day", tick->trading_day);
        dict_set_long(d, "update_time", tick->update_time);

        dict_set_double(d, "bid_price1", tick->bid_price[0]);
        dict_set_double(d, "bid_price2", tick->bid_price[1]);
        dict_set_double(d, "bid_price3", tick->bid_price[2]);
        dict_set_double(d, "bid_price4", tick->bid_price[3]);
        dict_set_double(d, "bid_price5", tick->bid_price[4]);

        dict_set_long(d, "bid_volume1", tick->bid_volume[0]);
        dict_set_long(d, "bid_volume2", tick->bid_volume[1]);
        dict_set_long(d, "bid_volume3", tick->bid_volume[2]);
        dict_set_long(d, "bid_volume4", tick->bid_volume[3]);
        dict_set_long(d, "bid_volume5", tick->bid_volume[4]);

        dict_set_double(d, "ask_price1", tick->ask_price[0]);
        dict_set_double(d, "ask_price2", tick->ask_price[1]);
        dict_set_double(d, "ask_price3", tick->ask_price[2]);
        dict_set_double(d, "ask_price4", tick->ask_price[3]);
        dict_set_double(d, "ask_price5", tick->ask_price[4]);

        dict_set_long(d, "ask_volume1", tick->ask_volume[0]);
        dict_set_long(d, "ask_volume2", tick->ask_volume[1]);
        dict_set_long(d, "ask_volume3", tick->ask_volume[2]);
        dict_set_long(d, "ask_volume4", tick->ask_volume[3]);
        dict_set_long(d, "ask_volume5", tick->ask_volume[4]);

        PyObject* ret = PyObject_CallFunctionObjArgs(py_on_tick_, d, nullptr);
        Py_DECREF(d);
        Py_XDECREF(ret);

        if (PyErr_Occurred()) {
            PyErr_Print();
            handle_error();
        }

        PyGILState_Release(gstate);
    }

    void on_kline(KlineRecord* kline) {
        if (!enabled_ || !py_on_kline_ || !kline) return;

        if (!symbol_filter_.empty() && symbol_filter_.find(kline->symbol_id) == symbol_filter_.end()) {
            return;
        }

        kline_count_++;
        if ((kline_count_ % sample_every_) != 0) return;

        PyGILState_STATE gstate = PyGILState_Ensure();

        PyObject* d = PyDict_New();
        dict_set_str(d, "symbol", kline->symbol);
        dict_set_long(d, "symbol_id", static_cast<long long>(kline->symbol_id));
        dict_set_long(d, "trading_day", kline->trading_day);
        dict_set_long(d, "start_time", kline->start_time);
        dict_set_double(d, "open", kline->open);
        dict_set_double(d, "high", kline->high);
        dict_set_double(d, "low", kline->low);
        dict_set_double(d, "close", kline->close);
        dict_set_long(d, "volume", kline->volume);
        dict_set_double(d, "turnover", kline->turnover);
        dict_set_double(d, "open_interest", kline->open_interest);
        dict_set_long(d, "interval", static_cast<long long>(kline->interval));

        PyObject* ret = PyObject_CallFunctionObjArgs(py_on_kline_, d, nullptr);
        Py_DECREF(d);
        Py_XDECREF(ret);

        if (PyErr_Occurred()) {
            PyErr_Print();
            handle_error();
        }

        PyGILState_Release(gstate);
    }

    void on_order(const OrderRtn* rtn) {
        if (!enabled_ || !py_on_order_ || !rtn) return;

        PyGILState_STATE gstate = PyGILState_Ensure();
        PyObject* d = PyDict_New();
        dict_set_str(d, "account_id", rtn->account_id);
        dict_set_str(d, "order_ref", rtn->order_ref);
        dict_set_str(d, "order_sys_id", rtn->order_sys_id);
        dict_set_str(d, "exchange_id", rtn->exchange_id);
        dict_set_str(d, "symbol", rtn->symbol);
        dict_set_long(d, "symbol_id", static_cast<long long>(rtn->symbol_id));
        dict_set_str(d, "direction", std::string(1, rtn->direction).c_str());
        dict_set_str(d, "offset_flag", std::string(1, rtn->offset_flag).c_str());
        dict_set_double(d, "limit_price", rtn->limit_price);
        dict_set_long(d, "volume_total", rtn->volume_total);
        dict_set_long(d, "volume_traded", rtn->volume_traded);
        dict_set_long(d, "insert_time", static_cast<long long>(rtn->insert_time));
        dict_set_long(d, "update_time", static_cast<long long>(rtn->update_time));
        dict_set_str(d, "status", std::string(1, rtn->status).c_str());
        dict_set_str(d, "status_msg", rtn->status_msg);
        PyObject* ret = PyObject_CallFunctionObjArgs(py_on_order_, d, nullptr);
        Py_DECREF(d);
        Py_XDECREF(ret);
        if (PyErr_Occurred()) {
            PyErr_Print();
            handle_error();
        }
        PyGILState_Release(gstate);
    }

    void on_trade(const TradeRtn* rtn) {
        if (!enabled_ || !py_on_trade_ || !rtn) return;

        PyGILState_STATE gstate = PyGILState_Ensure();
        PyObject* d = PyDict_New();
        dict_set_str(d, "account_id", rtn->account_id);
        dict_set_str(d, "exchange_id", rtn->exchange_id);
        dict_set_str(d, "symbol", rtn->symbol);
        dict_set_long(d, "symbol_id", static_cast<long long>(rtn->symbol_id));
        dict_set_str(d, "direction", std::string(1, rtn->direction).c_str());
        dict_set_str(d, "offset_flag", std::string(1, rtn->offset_flag).c_str());
        dict_set_double(d, "price", rtn->price);
        dict_set_long(d, "volume", rtn->volume);
        dict_set_long(d, "trade_time", static_cast<long long>(rtn->trade_time));
        dict_set_str(d, "trade_id", rtn->trade_id);
        dict_set_str(d, "liquidity_role", std::string(1, rtn->liquidity_role).c_str());
        dict_set_str(d, "order_ref", rtn->order_ref);
        dict_set_str(d, "order_sys_id", rtn->order_sys_id);
        PyObject* ret = PyObject_CallFunctionObjArgs(py_on_trade_, d, nullptr);
        Py_DECREF(d);
        Py_XDECREF(ret);
        if (PyErr_Occurred()) {
            PyErr_Print();
            handle_error();
        }
        PyGILState_Release(gstate);
    }

    void call_simple_hook(PyObject* hook) {
        if (!enabled_ || !hook) return;
        PyGILState_STATE gstate = PyGILState_Ensure();
        PyObject* ret = PyObject_CallFunctionObjArgs(hook, nullptr);
        Py_XDECREF(ret);
        if (PyErr_Occurred()) {
            PyErr_Print();
            handle_error();
        }
        PyGILState_Release(gstate);
    }

    void handle_error() {
        if (error_policy_ == "ignore") return;
        if (error_policy_ == "disable") {
            enabled_ = false;
            LOG_ERROR("[PyStrategy] Disabled after error.");
            return;
        }
        if (error_policy_ == "stop") {
            bus_->publish(EVENT_ENGINE_STOP, nullptr);
        }
    }

    std::string get_cfg(const ConfigMap& config, const std::string& key, const std::string& def) {
        auto it = config.find(key);
        if (it != config.end()) return it->second;
        return def;
    }

    EventBus* bus_ = nullptr;
    bool enabled_ = true;
    uint64_t tick_count_ = 0;
    uint64_t kline_count_ = 0;
    int sample_every_ = 1;
    FastHashSet<uint64_t> symbol_filter_;

    std::string py_module_;
    std::string py_class_;
    std::string py_path_;
    std::string default_account_;
    std::string error_policy_;

    PyObject* py_instance_ = nullptr;
    PyObject* py_on_init_ = nullptr;
    PyObject* py_on_start_ = nullptr;
    PyObject* py_on_tick_ = nullptr;
    PyObject* py_on_kline_ = nullptr;
    PyObject* py_on_order_ = nullptr;
    PyObject* py_on_trade_ = nullptr;
    PyObject* py_on_stop_ = nullptr;
    PyObject* py_send_order_ = nullptr;
    PyObject* py_cancel_order_ = nullptr;
};

EXPORT_MODULE(PyStrategyModule)
