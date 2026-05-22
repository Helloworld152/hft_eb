#include "engine.h"
#include "logging.h"

#include <pybind11/pybind11.h>

#include <memory>
#include <string>

namespace py = pybind11;

namespace {

class PyBacktestEngine {
public:
    PyBacktestEngine() : engine_(std::make_unique<HftEngine>()) {}

    bool load_config(const std::string& config_path, const std::string& logger_name = "hft_backtest") {
        config_path_ = config_path;
        hft::logging::init_logging_from_yaml_file(config_path, logger_name);
        return engine_->loadConfig(config_path);
    }

    void start() {
        engine_->start();
    }

    void run() {
        py::gil_scoped_release release;
        engine_->run();
    }

    void stop() {
        engine_->stop();
    }

    const std::string& config_path() const {
        return config_path_;
    }

    ~PyBacktestEngine() {
        if (engine_) {
            engine_->stop();
        }
        hft::logging::shutdown_logging();
    }

private:
    std::unique_ptr<HftEngine> engine_;
    std::string config_path_;
};

}  // namespace

PYBIND11_MODULE(_core, m) {
    m.doc() = "hft_backtest native bindings";

    py::class_<PyBacktestEngine>(m, "NativeBacktestEngine")
        .def(py::init<>())
        .def("load_config", &PyBacktestEngine::load_config,
             py::arg("config_path"),
             py::arg("logger_name") = "hft_backtest")
        .def("start", &PyBacktestEngine::start)
        .def("run", &PyBacktestEngine::run)
        .def("stop", &PyBacktestEngine::stop)
        .def_property_readonly("config_path", &PyBacktestEngine::config_path);
}
