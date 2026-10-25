// MetricsRegistry.h
#ifndef METRICS_REGISTRY_H
#define METRICS_REGISTRY_H

#include <map>
#include <string>
#include <sstream>

class MetricBase {
public:
    virtual ~MetricBase() {}
    virtual std::string toString() const = 0;
};

template <typename T>
class Metric : public MetricBase {
public:
    Metric(T* value) : value(value) {}
    std::string toString() const override {
        std::ostringstream oss;
        oss << (*value);
        return oss.str();
    }
private:
    T* value;
};

class MetricsRegistry {
public:
    ~MetricsRegistry() {
        for (auto& pair : metrics) {
            delete pair.second;
        }
    }

    template <typename T>
    void registerMetric(const std::string& metricname, const std::string& submetric, T* value) {
        metrics[metricname + "/" + submetric] = new Metric<T>(value);
    }
    

    void writeToFile(const std::string& filepath);

private:
    std::map<std::string, MetricBase*> metrics;
};

static MetricsRegistry globalMetricsRegistry;

#endif // METRICS_REGISTRY_H