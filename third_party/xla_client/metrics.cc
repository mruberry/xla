#include "tensorflow/compiler/xla/xla_client/metrics.h"

#include <cmath>
#include <map>
#include <sstream>

#include "tensorflow/core/platform/default/logging.h"
#include "tensorflow/core/platform/macros.h"

namespace xla {
namespace metrics {
namespace {

class MetricsArena {
 public:
  static MetricsArena* Get();

  // Registers a new metric in the global arena.
  void RegisterMetric(const string& name, MetricReprFn repr_fn,
                      size_t max_samples, std::shared_ptr<MetricData>* data);

  void RegisterCounter(const string& name, std::shared_ptr<CounterData>* data);

  void ForEachMetric(
      const std::function<void(const string&, MetricData*)>& metric_func);

  void ForEachCounter(
      const std::function<void(const string&, CounterData*)>& counter_func);

  std::vector<string> GetMetricNames() {
    std::vector<string> names;
    std::lock_guard<std::mutex> lock(lock_);
    for (auto& name_data : metrics_) {
      names.push_back(name_data.first);
    }
    return names;
  }

  MetricData* GetMetric(const string& name) {
    std::lock_guard<std::mutex> lock(lock_);
    auto it = metrics_.find(name);
    return it != metrics_.end() ? it->second.get() : nullptr;
  }

  std::vector<string> GetCounterNames() {
    std::vector<string> names;
    std::lock_guard<std::mutex> lock(lock_);
    for (auto& name_data : counters_) {
      names.push_back(name_data.first);
    }
    return names;
  }

  CounterData* GetCounter(const string& name) {
    std::lock_guard<std::mutex> lock(lock_);
    auto it = counters_.find(name);
    return it != counters_.end() ? it->second.get() : nullptr;
  }

 private:
  std::mutex lock_;
  std::map<string, std::shared_ptr<MetricData>> metrics_;
  std::map<string, std::shared_ptr<CounterData>> counters_;
};

MetricsArena* MetricsArena::Get() {
  static MetricsArena* arena = new MetricsArena();
  return arena;
}

void MetricsArena::RegisterMetric(const string& name, MetricReprFn repr_fn,
                                  size_t max_samples,
                                  std::shared_ptr<MetricData>* data) {
  std::lock_guard<std::mutex> lock(lock_);
  if (*data == nullptr) {
    std::shared_ptr<MetricData> new_data =
        std::make_shared<MetricData>(std::move(repr_fn), max_samples);
    auto it = metrics_.emplace(name, new_data).first;
    *data = it->second;
  }
}

void MetricsArena::RegisterCounter(const string& name,
                                   std::shared_ptr<CounterData>* data) {
  std::lock_guard<std::mutex> lock(lock_);
  if (*data == nullptr) {
    std::shared_ptr<CounterData> new_data = std::make_shared<CounterData>();
    auto it = counters_.emplace(name, new_data).first;
    *data = it->second;
  }
}

void MetricsArena::ForEachMetric(
    const std::function<void(const string&, MetricData*)>& metric_func) {
  std::lock_guard<std::mutex> lock(lock_);
  for (auto& name_data : metrics_) {
    metric_func(name_data.first, name_data.second.get());
  }
}

void MetricsArena::ForEachCounter(
    const std::function<void(const string&, CounterData*)>& counter_func) {
  std::lock_guard<std::mutex> lock(lock_);
  for (auto& name_data : counters_) {
    counter_func(name_data.first, name_data.second.get());
  }
}

void EmitMetricInfo(const string& name, MetricData* data,
                    std::stringstream* ss) {
  double accumulator = 0.0;
  size_t total_samples = 0;
  std::vector<Sample> samples = data->Samples(&accumulator, &total_samples);
  (*ss) << "Metric: " << name << std::endl;
  (*ss) << "  TotalSamples: " << total_samples << std::endl;
  (*ss) << "  Accumulator: " << data->Repr(accumulator) << std::endl;
  if (!samples.empty()) {
    double total = 0.0;
    for (auto& sample : samples) {
      total += sample.value;
    }
    int64 delta_time =
        samples.back().timestamp_ns - samples.front().timestamp_ns;
    if (delta_time > 0) {
      double value_sec = 1e6 * (total / (delta_time / 1000.0));
      (*ss) << "  ValueRate: " << data->Repr(value_sec) << " / second"
            << std::endl;
      double count_sec =
          1e6 * (static_cast<double>(samples.size()) / (delta_time / 1000.0));
      (*ss) << "  Rate: " << count_sec << " / second" << std::endl;
    }
  }

  const int kNumPercentiles = 9;
  static double const kPercentiles[kNumPercentiles] = {
      0.01, 0.05, 0.1, 0.2, 0.5, 0.8, 0.9, 0.95, 0.99};
  std::sort(
      samples.begin(), samples.end(),
      [](const Sample& s1, const Sample& s2) { return s1.value < s2.value; });
  (*ss) << "  Percentiles: ";
  for (int i = 0; i < kNumPercentiles; ++i) {
    size_t index = kPercentiles[i] * samples.size();
    if (i > 0) {
      (*ss) << "; ";
    }
    (*ss) << (kPercentiles[i] * 100.0)
          << "%=" << data->Repr(samples[index].value);
  }
  (*ss) << std::endl;
}

void EmitCounterInfo(const string& name, CounterData* data,
                     std::stringstream* ss) {
  (*ss) << "Counter: " << name << std::endl;
  (*ss) << "  Value: " << data->Value() << std::endl;
}

}  // namespace

MetricData::MetricData(MetricReprFn repr_fn, size_t max_samples)
    : repr_fn_(std::move(repr_fn)), samples_(max_samples) {}

void MetricData::AddSample(int64 timestamp_ns, double value) {
  std::lock_guard<std::mutex> lock(lock_);
  size_t position = count_ % samples_.size();
  ++count_;
  accumulator_ += value;
  samples_[position] = Sample(timestamp_ns, value);
}

double MetricData::Accumulator() const {
  std::lock_guard<std::mutex> lock(lock_);
  return accumulator_;
}

size_t MetricData::TotalSamples() const {
  std::lock_guard<std::mutex> lock(lock_);
  return count_;
}

std::vector<Sample> MetricData::Samples(double* accumulator,
                                        size_t* total_samples) const {
  std::lock_guard<std::mutex> lock(lock_);
  std::vector<Sample> samples;
  if (count_ <= samples_.size()) {
    samples.insert(samples.end(), samples_.begin(), samples_.begin() + count_);
  } else {
    size_t position = count_ % samples_.size();
    samples.insert(samples.end(), samples_.begin() + position, samples_.end());
    samples.insert(samples.end(), samples_.begin(),
                   samples_.begin() + position);
  }
  if (accumulator != nullptr) {
    *accumulator = accumulator_;
  }
  if (total_samples != nullptr) {
    *total_samples = count_;
  }
  return samples;
}

Metric::Metric(string name, MetricReprFn repr_fn, size_t max_samples)
    : name_(std::move(name)),
      repr_fn_(std::move(repr_fn)),
      max_samples_(max_samples),
      data_(nullptr) {}

double Metric::Accumulator() const { return GetData()->Accumulator(); }

void Metric::AddSample(int64 timestamp_ns, double value) {
  GetData()->AddSample(timestamp_ns, value);
}

void Metric::AddSample(double value) {
  GetData()->AddSample(sys_util::NowNs(), value);
}

std::vector<Sample> Metric::Samples(double* accumulator,
                                    size_t* total_samples) const {
  return GetData()->Samples(accumulator, total_samples);
}

string Metric::Repr(double value) const { return GetData()->Repr(value); }

MetricData* Metric::GetData() const {
  MetricData* data = data_.load();
  if (TF_PREDICT_FALSE(data == nullptr)) {
    // The RegisterMetric() API is a synchronization point, and even if multiple
    // threads enters it, the data will be created only once.
    MetricsArena* arena = MetricsArena::Get();
    arena->RegisterMetric(name_, repr_fn_, max_samples_, &data_ptr_);
    // Even if multiple threads will enter this IF statement, they will all
    // fetch the same value, and hence store the same value below.
    data = data_ptr_.get();
    data_.store(data);
  }
  return data;
}

Counter::Counter(string name) : name_(std::move(name)), data_(nullptr) {}

CounterData* Counter::GetData() const {
  CounterData* data = data_.load();
  if (TF_PREDICT_FALSE(data == nullptr)) {
    // The RegisterCounter() API is a synchronization point, and even if
    // multiple threads enters it, the data will be created only once.
    MetricsArena* arena = MetricsArena::Get();
    arena->RegisterCounter(name_, &data_ptr_);
    // Even if multiple threads will enter this IF statement, they will all
    // fetch the same value, and hence store the same value below.
    data = data_ptr_.get();
    data_.store(data);
  }
  return data;
}

string MetricFnValue(double value) {
  std::stringstream ss;
  ss.precision(2);
  ss << std::fixed << value;
  return ss.str();
}

string MetricFnBytes(double value) {
  const int kNumSuffixes = 6;
  static const char* const kSizeSuffixes[kNumSuffixes] = {"B",  "KB", "MB",
                                                          "GB", "TB", "PB"};
  int sfix = 0;
  for (; (sfix + 1) < kNumSuffixes && value >= 1024.0; ++sfix) {
    value /= 1024.0;
  }
  std::stringstream ss;
  ss.precision(2);
  ss << std::fixed << value << kSizeSuffixes[sfix];
  return ss.str();
}

string MetricFnTime(double value) {
  static struct TimePart {
    const char* suffix;
    double scaler;
    int width;
    int precision;
    char fill;
  } const time_parts[] = {
      {"d", 86400.0 * 1e9, 2, 0, '0'}, {"h", 1440.0 * 1e9, 2, 0, '0'},
      {"m", 60.0 * 1e9, 2, 0, '0'},    {"s", 1e9, 2, 0, '0'},
      {"ms", 1e6, 3, 0, '0'},          {"us", 1e3, 3, 3, '0'},
  };
  int count = 0;
  std::stringstream ss;
  for (auto& part : time_parts) {
    double ctime = value / part.scaler;
    if (ctime >= 1.0 || count > 0) {
      ss.precision(part.precision);
      ss.width(part.width);
      ss.fill(part.fill);
      ss << std::fixed << ctime << part.suffix;
      value -= std::floor(ctime) * part.scaler;
      ++count;
    }
  }
  return ss.str();
}

string CreateMetricReport() {
  MetricsArena* arena = MetricsArena::Get();
  std::stringstream ss;
  arena->ForEachMetric([&ss](const string& name, MetricData* data) {
    EmitMetricInfo(name, data, &ss);
  });
  arena->ForEachCounter([&ss](const string& name, CounterData* data) {
    EmitCounterInfo(name, data, &ss);
  });
  return ss.str();
}

std::vector<string> GetMetricNames() {
  return MetricsArena::Get()->GetMetricNames();
}

MetricData* GetMetric(const string& name) {
  return MetricsArena::Get()->GetMetric(name);
}

std::vector<string> GetCounterNames() {
  return MetricsArena::Get()->GetCounterNames();
}

CounterData* GetCounter(const string& name) {
  return MetricsArena::Get()->GetCounter(name);
}

}  // namespace metrics
}  // namespace xla
