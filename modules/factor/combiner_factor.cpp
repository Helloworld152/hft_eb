#include "../../include/factor/factor_node.h"
#include <yaml-cpp/yaml.h>
#include <vector>
#include <string>

class LinearCombinerFactor : public IFactorNode {
public:
    void init(const ConfigMap& config) override {
        auto it_dbg = config.find("debug");
        if (it_dbg != config.end()) {
            debug_ = (it_dbg->second == "true");
        }

        auto it_yaml = config.find("_yaml");
        if (it_yaml == config.end()) {
            return;
        }

        try {
            YAML::Node node = YAML::Load(it_yaml->second);
            if (node["weights"] && node["weights"].IsSequence()) {
                weights_.clear();
                for (const auto& w : node["weights"]) {
                    weights_.push_back(w.as<double>());
                }
            }
        } catch (const YAML::Exception& e) {
            LOG_ERROR("[CombinerFactor] YAML parse error: {}", e.what());
        }
    }

    double compute(const FactorContext& /*ctx*/, const std::vector<double>& inputs) override {
        if (inputs.empty()) return 0.0;
        if (!weights_.empty() && weights_.size() != inputs.size()) {
            if (!warned_mismatch_) {
                LOG_WARN("[CombinerFactor] weights size mismatch: weights={} inputs={}",
                         weights_.size(),
                         inputs.size());
                warned_mismatch_ = true;
            }
            return 0.0;
        }

        double sum = 0.0;
        if (weights_.empty()) {
            for (double v : inputs) sum += v;
        } else {
            for (size_t i = 0; i < inputs.size(); ++i) {
                sum += inputs[i] * weights_[i];
            }
        }

        if (debug_) {
            LOG_DEBUG("[CombinerFactor] value={} inputs={}", sum, inputs.size());
        }
        return sum;
    }

private:
    std::vector<double> weights_;
    bool debug_ = false;
    bool warned_mismatch_ = false;
};

EXPORT_FACTOR(LinearCombinerFactor)
