#include "framework.h"
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

        bus_->subscribe(EVENT_MARKET_DATA, [this](void* d) {
            this->on_tick(static_cast<TickRecord*>(d));
        });

        if (py_on_kline_) {
            bus_->subscribe(EVENT_KLINE, [this](void* d) {
                this->on_kline(static_cast<KlineRecord*>(d));
            });
        }

        LOG_INFO("[PyStrategy] Initialized. module={} class={} on_tick={} on_kline={}",
                 py_module_,
                 py_class_,
                 py_on_tick_ ? "yes" : "no",
                 py_on_kline_ ? "yes" : "no");
    }

    void stop() override {
        PyGILState_STATE gstate = PyGILState_Ensure();
        Py_XDECREF(py_on_tick_);
        Py_XDECREF(py_on_kline_);
        Py_XDECREF(py_instance_);
        Py_XDECREF(py_send_order_);
        PyGILState_Release(gstate);
    }

private:
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
        }

        mod->bus_->publish(EVENT_ORDER_REQ, &req);
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
        static PyMethodDef def = {
            "send_order",
            (PyCFunction)py_send_order,
            METH_VARARGS | METH_KEYWORDS,
            "send order to engine"
        };
        py_send_order_ = PyCFunction_NewEx(&def, capsule, nullptr);
        Py_DECREF(capsule);

        bool ctor_with_args = true;
        py_instance_ = PyObject_CallFunctionObjArgs(cls, config_dict, py_send_order_, nullptr);

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

        if (!ctor_with_args && PyObject_HasAttrString(py_instance_, "init")) {
            PyObject* init_fn = PyObject_GetAttrString(py_instance_, "init");
            if (init_fn && PyCallable_Check(init_fn)) {
                PyObject* args = PyTuple_Pack(2, config_dict, py_send_order_);
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

        PyErr_Clear();
        py_on_tick_ = PyObject_GetAttrString(py_instance_, "on_tick");
        if (py_on_tick_ && !PyCallable_Check(py_on_tick_)) {
            Py_DECREF(py_on_tick_);
            py_on_tick_ = nullptr;
        }
        PyErr_Clear();

        py_on_kline_ = PyObject_GetAttrString(py_instance_, "on_kline");
        if (py_on_kline_ && !PyCallable_Check(py_on_kline_)) {
            Py_DECREF(py_on_kline_);
            py_on_kline_ = nullptr;
        }
        PyErr_Clear();

        Py_DECREF(cls);
        Py_DECREF(module);

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
    PyObject* py_on_tick_ = nullptr;
    PyObject* py_on_kline_ = nullptr;
    PyObject* py_send_order_ = nullptr;
};

EXPORT_MODULE(PyStrategyModule)
