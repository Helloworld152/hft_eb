#include "../../include/factor/factor_node.h"
#include <yaml-cpp/yaml.h>
#include <iostream>
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
            std::cerr << "[CombinerFactor] YAML parse error: " << e.what() << std::endl;
        }
    }

    double compute(const FactorContext& /*ctx*/, const std::vector<double>& inputs) override {
        if (inputs.empty()) return 0.0;
        if (!weights_.empty() && weights_.size() != inputs.size()) {
            if (!warned_mismatch_) {
                std::cerr << "[CombinerFactor] weights size mismatch: weights="
                          << weights_.size() << " inputs=" << inputs.size() << std::endl;
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
            std::cout << "[CombinerFactor] value=" << sum << " inputs=" << inputs.size() << std::endl;
        }
        return sum;
    }

private:
    std::vector<double> weights_;
    bool debug_ = false;
    bool warned_mismatch_ = false;
};

EXPORT_FACTOR(LinearCombinerFactor)
