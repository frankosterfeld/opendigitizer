#ifndef OPENDIGITIZER_SERVICE_GNURADIOWORKER_H
#define OPENDIGITIZER_SERVICE_GNURADIOWORKER_H

#include "gnuradio-4.0/Message.hpp"
#include <daq_api.hpp>

#include <majordomo/Worker.hpp>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Graph_yaml_importer.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/basic/DataSink.hpp>

#include <chrono>
#include <memory>
#include <ranges>
#include <string_view>
#include <utility>

namespace opendigitizer::acq {

namespace detail {
template<typename T>
inline std::optional<T> get(const gr::property_map& m, const std::string_view& key) {
    const auto it = m.find(std::string(key));
    if (it == m.end()) {
        return {};
    }

    try {
        return std::get<T>(it->second);
    } catch (const std::exception& e) {
        fmt::println(std::cerr, "Unexpected type for tag '{}'", key);
        return {};
    }
}
inline float doubleToFloat(double v) { return static_cast<float>(v); }

inline std::string findTriggerName(std::span<const gr::Tag> tags) {
    for (const auto& tag : tags) {
        const auto v = tag.get(std::string(gr::tag::TRIGGER_NAME.key()));
        if (!v) {
            continue;
        }
        return std::get<std::string>(v->get());
    }
    return {};
}

template<typename T>
inline std::optional<T> getSetting(const gr::BlockModel& block, const std::string& key) {
    try {
        const auto setting = block.settings().get(key);
        if (!setting) {
            return {};
        }
        return std::get<T>(*setting);
    } catch (const std::exception& e) {
        fmt::println(std::cerr, "Unexpected type for '{}' property", key);
        return {};
    }
}

} // namespace detail

using namespace gr;
using namespace gr::message;
using enum gr::message::Command;
using namespace opencmw::majordomo;
using namespace std::chrono_literals;

enum class AcquisitionMode { Continuous, Triggered, Multiplexed, Snapshot, DataSet };

constexpr inline AcquisitionMode parseAcquisitionMode(std::string_view v) {
    using enum AcquisitionMode;
    if (v == "continuous") {
        return Continuous;
    }
    if (v == "triggered") {
        return Triggered;
    }
    if (v == "multiplexed") {
        return Multiplexed;
    }
    if (v == "snapshot") {
        return Snapshot;
    }
    if (v == "dataset") {
        return DataSet;
    }
    throw std::invalid_argument(fmt::format("Invalid acquisition mode '{}'", v));
}

struct PollerKey {
    AcquisitionMode          mode;
    std::string              signal_name;
    std::size_t              pre_samples         = 0;                           // Trigger
    std::size_t              post_samples        = 0;                           // Trigger
    std::size_t              maximum_window_size = 0;                           // Multiplexed
    std::chrono::nanoseconds snapshot_delay      = std::chrono::nanoseconds(0); // Snapshot
    std::string              trigger_name        = {};                          // Trigger, Multiplexed, Snapshot

    auto operator<=>(const PollerKey&) const noexcept = default;
};

struct StreamingPollerEntry {
    using SampleType                                               = double;
    bool                                                    in_use = true;
    std::shared_ptr<gr::basic::StreamingPoller<SampleType>> poller;
    std::optional<std::string>                              signal_name;
    std::optional<std::string>                              signal_unit;
    std::optional<float>                                    signal_min;
    std::optional<float>                                    signal_max;

    explicit StreamingPollerEntry(std::shared_ptr<basic::StreamingPoller<SampleType>> p) : poller{p} {}

    void populateFromTags(std::span<const gr::Tag>& tags) {
        for (const auto& tag : tags) {
            if (const auto name = detail::get<std::string>(tag.map, tag::SIGNAL_NAME.shortKey())) {
                signal_name = name;
            }
            if (const auto unit = detail::get<std::string>(tag.map, tag::SIGNAL_UNIT.shortKey())) {
                signal_unit = unit;
            }
            if (const auto min = detail::get<float>(tag.map, tag::SIGNAL_MIN.shortKey())) {
                signal_min = min;
            }
            if (const auto max = detail::get<float>(tag.map, tag::SIGNAL_MAX.shortKey())) {
                signal_max = max;
            }
        }
    }
};

struct SignalEntry {
    std::string name;
    std::string unit;
    float       sample_rate;

    auto operator<=>(const SignalEntry&) const noexcept = default;
};

struct DataSetPollerEntry {
    using SampleType = double;
    std::shared_ptr<gr::basic::DataSetPoller<SampleType>> poller;
    bool                                                  in_use = false;
};

template<units::basic_fixed_string serviceName, typename... Meta>
class GnuRadioAcquisitionWorker : public Worker<serviceName, TimeDomainContext, Empty, Acquisition, Meta...> {
    gr::PluginLoader*                             _plugin_loader;
    std::jthread                                  _notifyThread;
    std::unique_ptr<gr::Graph>                    _pending_flow_graph;
    std::mutex                                    _flow_graph_mutex;
    std::function<void(std::vector<SignalEntry>)> _updateSignalEntriesCallback;

public:
    using super_t = Worker<serviceName, TimeDomainContext, Empty, Acquisition, Meta...>;

    explicit GnuRadioAcquisitionWorker(opencmw::URI<opencmw::STRICT> brokerAddress, const opencmw::zmq::Context& context, gr::PluginLoader* pluginLoader, std::chrono::milliseconds rate, Settings settings = {}) : super_t(std::move(brokerAddress), {}, context, std::move(settings)), _plugin_loader(pluginLoader) {
        // TODO would be useful if one can check if the external broker knows TimeDomainContext and throw an error if not
        init(rate);
    }

    template<typename BrokerType>
    explicit GnuRadioAcquisitionWorker(BrokerType& broker, gr::PluginLoader* pluginLoader, std::chrono::milliseconds rate) : super_t(broker, {}), _plugin_loader(pluginLoader) {
        // this makes sure the subscriptions are filtered correctly
        opencmw::query::registerTypes(TimeDomainContext(), broker);
        init(rate);
    }

    ~GnuRadioAcquisitionWorker() {
        _notifyThread.request_stop();
        _notifyThread.join();
    }

    void setGraph(std::unique_ptr<gr::Graph> fg) {
        std::lock_guard lg{_flow_graph_mutex};
        _pending_flow_graph = std::move(fg);
    }

    void setUpdateSignalEntriesCallback(std::function<void(std::vector<SignalEntry>)> callback) { _updateSignalEntriesCallback = std::move(callback); }

private:
    void init(std::chrono::milliseconds rate) {
        // TODO instead of a notify thread with polling, we could also use callbacks. This would require
        // the ability to unregister callbacks though (RAII callback "handles" using shared_ptr/weak_ptr like it works for pollers??)
        _notifyThread = std::jthread([this, rate](const std::stop_token& stoken) {
            auto update = std::chrono::system_clock::now();
            // TODO: current load_grc creates Foo<double> types no matter what the original type was
            // when supporting more types, we need some type erasure here
            std::map<PollerKey, StreamingPollerEntry>       streamingPollers;
            std::map<PollerKey, DataSetPollerEntry>         dataSetPollers;
            std::jthread                                    schedulerThread;
            std::string                                     schedulerUniqueName;
            std::map<std::string, std::vector<SignalEntry>> signalEntriesBySink;
            std::unique_ptr<MsgPortOut>                     toScheduler;
            std::unique_ptr<MsgPortIn>                      fromScheduler;

            bool finished = false;

            while (!finished) {
                const auto aboutToFinish    = stoken.stop_requested();
                auto       pendingFlowGraph = [this]() {
                    std::lock_guard lg{_flow_graph_mutex};
                    return std::exchange(_pending_flow_graph, {});
                }();
                const auto hasScheduler      = schedulerThread.joinable();
                const bool stopScheduler     = hasScheduler && (aboutToFinish || pendingFlowGraph);
                bool       schedulerFinished = false;

                if (stopScheduler) {
                    sendMessage<Set>(*toScheduler, schedulerUniqueName, block::property::kLifeCycleState, {{"state", std::string(magic_enum::enum_name(lifecycle::State::REQUESTED_STOP))}}, "");
                }

                if (hasScheduler) {
                    bool signalInfoChanged = false;
                    auto messages          = fromScheduler->streamReader().get(fromScheduler->streamReader().available());
                    for (const auto& message : messages) {
                        if (message.endpoint == block::property::kLifeCycleState) {
                            if (!message.data) {
                                continue;
                            }
                            const auto state = detail::get<std::string>(*message.data, "state");
                            if (state == magic_enum::enum_name(lifecycle::State::STOPPED)) {
                                schedulerFinished = true;
                                continue;
                            }
                        } else if (message.endpoint == block::property::kSetting) {
                            auto sinkIt = signalEntriesBySink.find(message.serviceName);
                            if (sinkIt == signalEntriesBySink.end()) {
                                continue;
                            }
                            const auto& settings = message.data;
                            if (!settings) {
                                continue;
                            }
                            auto& entries = sinkIt->second;

                            const auto signal_names = detail::get<std::vector<std::string>>(*settings, "signal_names");
                            const auto signal_units = detail::get<std::vector<std::string>>(*settings, "signal_units");

                            if (signal_names && signal_units) {

                            } else {
                                entries.resize(1);
                                auto&      entry       = entries[0];
                                const auto signal_name = detail::get<std::string>(*settings, "signal_name");
                                const auto signal_unit = detail::get<std::string>(*settings, "signal_unit");
                                const auto sample_rate = detail::get<float>(*settings, "sample_rate");
                                if (signal_name && signal_name != entry.name) {
                                    entry.name        = *signal_name;
                                    signalInfoChanged = true;
                                }
                                if (signal_unit && signal_unit != entry.unit) {
                                    entry.unit        = *signal_unit;
                                    signalInfoChanged = true;
                                }
                                if (sample_rate && sample_rate != entry.sample_rate) {
                                    entry.sample_rate = *sample_rate;
                                    signalInfoChanged = true;
                                }
                            }
                        }
                    }

                    std::ignore = messages.consume(messages.size());

                    if (signalInfoChanged && _updateSignalEntriesCallback) {
                        auto flattened = signalEntriesBySink | std::views::values | std::views::join;
                        _updateSignalEntriesCallback(std::vector(flattened.begin(), flattened.end()));
                    }

                    bool pollersFinished = true;
                    do {
                        pollersFinished = true;
                        for (auto& [_, pollerEntry] : streamingPollers) {
                            pollerEntry.in_use = false;
                        }
                        for (auto& [_, pollerEntry] : dataSetPollers) {
                            pollerEntry.in_use = false;
                        }
                        pollersFinished = handleSubscriptions(streamingPollers, dataSetPollers);
                        // drop pollers of old subscriptions to avoid the sinks from blocking
                        std::erase_if(streamingPollers, [](const auto& item) { return !item.second.in_use; });
                        std::erase_if(dataSetPollers, [](const auto& item) { return !item.second.in_use; });
                    } while (stopScheduler && !pollersFinished);
                }

                if (stopScheduler || schedulerFinished) {
                    if (_updateSignalEntriesCallback) {
                        _updateSignalEntriesCallback({});
                    }
                    signalEntriesBySink.clear();
                    streamingPollers.clear();
                    dataSetPollers.clear();
                    fromScheduler.reset();
                    toScheduler.reset();
                    schedulerUniqueName.clear();
                    schedulerThread.join();
                }

                if (aboutToFinish) {
                    finished = true;
                    continue;
                }

                if (pendingFlowGraph) {
                    pendingFlowGraph->forEachBlock([&signalEntriesBySink](const auto& block) {
                        if (block.typeName().starts_with("gr::basic::DataSink")) {
                            auto& entries = signalEntriesBySink[std::string(block.uniqueName())];
                            entries.resize(1);
                            auto& entry       = entries[0];
                            entry.name        = detail::getSetting<std::string>(block, "signal_name").value_or("");
                            entry.unit        = detail::getSetting<std::string>(block, "signal_unit").value_or("");
                            entry.sample_rate = detail::getSetting<float>(block, "sample_rate").value_or(1.f);
                        } else if (block.typeName().starts_with("gr::basic::DataSetSink")) {
                            auto&      entries = signalEntriesBySink[std::string(block.uniqueName())];
                            const auto names   = detail::getSetting<std::vector<std::string>>(block, "signal_names").value();
                            const auto units   = detail::getSetting<std::vector<std::string>>(block, "signal_units").value();
                            const auto n       = std::min(names.size(), units.size());
                            entries.resize(n);
                            for (auto i = 0UZ; i < n; ++i) {
                                entries[i].name        = names[i];
                                entries[i].unit        = units[i];
                                entries[i].sample_rate = 1.f; // no sample rate information available for data sets
                            }
                        }
                    });
                    if (_updateSignalEntriesCallback) {
                        auto flattened = signalEntriesBySink | std::views::values | std::views::join;
                        _updateSignalEntriesCallback(std::vector(flattened.begin(), flattened.end()));
                    }
                    auto sched          = std::make_unique<scheduler::Simple<scheduler::ExecutionPolicy::multiThreaded>>(std::move(*pendingFlowGraph));
                    toScheduler         = std::make_unique<MsgPortOut>();
                    fromScheduler       = std::make_unique<MsgPortIn>();
                    std::ignore         = toScheduler->connect(sched->msgIn);
                    std::ignore         = sched->msgOut.connect(*fromScheduler);
                    schedulerUniqueName = sched->unique_name;
                    sendMessage<Subscribe>(*toScheduler, schedulerUniqueName, block::property::kLifeCycleState, {}, "GnuRadioWorker");
                    sendMessage<Subscribe>(*toScheduler, "", block::property::kSetting, {}, "GnuRadioWorker");
                    schedulerThread = std::jthread([s = std::move(sched)] { s->runAndWait(); });
                }

                const auto next_update = update + rate;
                const auto now         = std::chrono::system_clock::now();
                if (now < next_update) {
                    std::this_thread::sleep_for(next_update - now);
                }
                update = next_update;
            }
        });
    }

    bool handleSubscriptions(std::map<PollerKey, StreamingPollerEntry>& streamingPollers, std::map<PollerKey, DataSetPollerEntry>& dataSetPollers) {
        bool pollersFinished = true;
        for (const auto& subscription : super_t::activeSubscriptions()) {
            const auto filterIn = opencmw::query::deserialise<TimeDomainContext>(subscription.params());
            try {
                const auto acquisitionMode = parseAcquisitionMode(filterIn.acquisitionModeFilter);
                for (std::string_view signalName : filterIn.channelNameFilter | std::ranges::views::split(',') | std::ranges::views::transform([](const auto&& r) { return std::string_view{&*r.begin(), std::ranges::distance(r)}; })) {
                    if (acquisitionMode == AcquisitionMode::Continuous) {
                        if (!handleStreamingSubscription(streamingPollers, filterIn, signalName)) {
                            pollersFinished = false;
                        }
                    } else {
                        if (!handleDataSetSubscription(dataSetPollers, filterIn, acquisitionMode, signalName)) {
                            pollersFinished = false;
                        }
                    }
                }
            } catch (const std::exception& e) {
                fmt::println(std::cerr, "Could not handle subscription {}: {}", subscription.toZmqTopic(), e.what());
            }
        }
        return pollersFinished;
    }

    auto getStreamingPoller(std::map<PollerKey, StreamingPollerEntry>& pollers, std::string_view signalName) {
        const auto key = PollerKey{.mode = AcquisitionMode::Continuous, .signal_name = std::string(signalName)};

        auto pollerIt = pollers.find(key);
        if (pollerIt == pollers.end()) {
            const auto query = basic::DataSinkQuery::signalName(signalName);
            pollerIt         = pollers.emplace(key, basic::DataSinkRegistry::instance().getStreamingPoller<double>(query)).first;
        }
        return pollerIt;
    }

    bool handleStreamingSubscription(std::map<PollerKey, StreamingPollerEntry>& pollers, const TimeDomainContext& context, std::string_view signalName) {
        auto pollerIt = getStreamingPoller(pollers, signalName);
        if (pollerIt == pollers.end()) { // flushing, do not create new pollers
            return true;
        }

        const auto& key         = pollerIt->first;
        auto&       pollerEntry = pollerIt->second;

        if (!pollerEntry.poller) {
            return true;
        }
        Acquisition reply;

        auto processData = [&reply, signalName, &pollerEntry](std::span<const double> data, std::span<const gr::Tag> tags) {
            pollerEntry.populateFromTags(tags);
            reply.acqTriggerName = "STREAMING";
            reply.channelName    = pollerEntry.signal_name.value_or(std::string(signalName));
            reply.channelUnit    = pollerEntry.signal_unit.value_or("N/A");
            // work around fix the Annotated::operator= ambiguity here (move vs. copy assignment) when creating a temporary unit here
            // Should be fixed in Annotated (templated forwarding assignment operator=?)/or go for gnuradio4's Annotated?
            const typename decltype(reply.channelRangeMin)::R rangeMin = pollerEntry.signal_min ? static_cast<float>(*pollerEntry.signal_min) : std::numeric_limits<float>::lowest();
            const typename decltype(reply.channelRangeMax)::R rangeMax = pollerEntry.signal_max ? static_cast<float>(*pollerEntry.signal_max) : std::numeric_limits<float>::max();
            reply.channelRangeMin                                      = rangeMin;
            reply.channelRangeMax                                      = rangeMax;
            reply.channelValue.resize(data.size());
            reply.channelError.resize(data.size());
            reply.channelTimeBase.resize(data.size());
            std::ranges::transform(data, reply.channelValue.begin(), detail::doubleToFloat);
            std::ranges::copy(data, reply.channelValue.begin());
            std::fill(reply.channelError.begin(), reply.channelError.end(), 0.f);     // TODO
            std::fill(reply.channelTimeBase.begin(), reply.channelTimeBase.end(), 0); // TODO
        };
        pollerEntry.in_use = true;

        const auto wasFinished = pollerEntry.poller->finished.load();
        if (pollerEntry.poller->process(processData)) {
            super_t::notify(context, reply);
        }
        return wasFinished;
    }

    auto getDataSetPoller(std::map<PollerKey, DataSetPollerEntry>& pollers, const TimeDomainContext& context, AcquisitionMode mode, std::string_view signalName) {
        const auto key = PollerKey{.mode = mode, .signal_name = std::string(signalName), .pre_samples = static_cast<std::size_t>(context.preSamples), .post_samples = static_cast<std::size_t>(context.postSamples), .maximum_window_size = static_cast<std::size_t>(context.maximumWindowSize), .snapshot_delay = std::chrono::nanoseconds(context.snapshotDelay), .trigger_name = context.triggerNameFilter};

        auto pollerIt = pollers.find(key);
        if (pollerIt == pollers.end()) {
            auto matcher = [trigger_name = context.triggerNameFilter](std::string_view, const gr::Tag& tag, const gr::property_map&) {
                using enum gr::trigger::MatchResult;
                const auto v = tag.get(gr::tag::TRIGGER_NAME);
                if (trigger_name.empty()) {
                    return v ? Matching : Ignore;
                }
                try {
                    if (!v) {
                        return Ignore;
                    }
                    return std::get<std::string>(v->get()) == trigger_name ? Matching : NotMatching;
                } catch (...) {
                    return NotMatching;
                }
            };
            const auto query = basic::DataSinkQuery::signalName(signalName);
            // TODO for triggered/multiplexed subscriptions that only differ in preSamples/postSamples/maximumWindowSize, we could use a single poller for the encompassing range
            // and send snippets from their datasets to the individual subscribers
            if (mode == AcquisitionMode::Triggered) {
                pollerIt = pollers.emplace(key, basic::DataSinkRegistry::instance().getTriggerPoller<double>(query, std::move(matcher), key.pre_samples, key.post_samples)).first;
            } else if (mode == AcquisitionMode::Snapshot) {
                pollerIt = pollers.emplace(key, basic::DataSinkRegistry::instance().getSnapshotPoller<double>(query, std::move(matcher), key.snapshot_delay)).first;
            } else if (mode == AcquisitionMode::Multiplexed) {
                pollerIt = pollers.emplace(key, basic::DataSinkRegistry::instance().getMultiplexedPoller<double>(query, std::move(matcher), key.maximum_window_size)).first;
            } else if (mode == AcquisitionMode::DataSet) {
                pollerIt = pollers.emplace(key, basic::DataSinkRegistry::instance().getDataSetPoller<double>(query)).first;
            }
        }
        return pollerIt;
    }

    bool handleDataSetSubscription(std::map<PollerKey, DataSetPollerEntry>& pollers, const TimeDomainContext& context, AcquisitionMode mode, std::string_view signalName) {
        auto pollerIt = getDataSetPoller(pollers, context, mode, signalName);
        if (pollerIt == pollers.end()) { // flushing, do not create new pollers
            return true;
        }

        const auto& key         = pollerIt->first;
        auto&       pollerEntry = pollerIt->second;

        if (!pollerEntry.poller) {
            return true;
        }
        Acquisition reply;
        auto        processData = [&reply, &key, signalName, &pollerEntry](std::span<const gr::DataSet<double>> dataSets) {
            const auto& dataSet = dataSets[0];
            const auto  signal_it = std::ranges::find(dataSet.signal_names, signalName);
            if (key.mode == AcquisitionMode::DataSet && signal_it == dataSet.signal_names.end()) {
                return;
            }
            const auto signal_idx = signal_it == dataSet.signal_names.end() ? 0UZ : std::distance(dataSet.signal_names.begin(), signal_it);
            if (!dataSet.timing_events.empty()) {
                reply.acqTriggerName = detail::findTriggerName(dataSet.timing_events[signal_idx]);
            }
            reply.channelName = dataSet.signal_names.empty() ? std::string(signalName) : dataSet.signal_names[signal_idx];
            reply.channelUnit = (dataSet.signal_units.size() < signal_idx - 1) ? "N/A" : dataSet.signal_units[signal_idx];
            if ((dataSet.signal_ranges.size() >= signal_idx) && dataSet.signal_ranges[signal_idx].size() == 2) {
                // Workaround for Annotated, see above
                const typename decltype(reply.channelRangeMin)::R rangeMin = dataSet.signal_ranges[signal_idx][0];
                const typename decltype(reply.channelRangeMax)::R rangeMax = dataSet.signal_ranges[signal_idx][1];
                reply.channelRangeMin                                      = rangeMin;
                reply.channelRangeMax                                      = rangeMax;
            }
            auto values = std::span(dataSet.signal_values);
            auto errors = std::span(dataSet.signal_errors);

            if (key.mode == AcquisitionMode::DataSet) {
                const auto samples = static_cast<std::size_t>(dataSet.extents[1]);
                const auto offset  = signal_idx * samples;
                const auto nValues = offset + samples <= values.size() ? samples : 0;
                const auto nErrors = offset + samples <= errors.size() ? samples : 0;
                values             = values.subspan(offset, nValues);
                errors             = errors.subspan(offset, nErrors);
            }

            reply.channelValue.resize(values.size());
            std::ranges::transform(values, reply.channelValue.begin(), detail::doubleToFloat);
            reply.channelError.resize(errors.size());
            std::ranges::transform(errors, reply.channelError.begin(), detail::doubleToFloat);
            reply.channelTimeBase.resize(values.size());
            std::fill(reply.channelTimeBase.begin(), reply.channelTimeBase.end(), 0); // TODO
        };
        pollerEntry.in_use = true;

        const auto wasFinished = pollerEntry.poller->finished.load();
        while (pollerEntry.poller->process(processData, 1)) {
            super_t::notify(context, reply);
        }

        return wasFinished;
    }
};

template<typename TAcquisitionWorker, units::basic_fixed_string serviceName, typename... Meta>
class GnuRadioFlowGraphWorker : public Worker<serviceName, flowgraph::FilterContext, flowgraph::Flowgraph, flowgraph::Flowgraph, Meta...> {
    gr::PluginLoader*    _plugin_loader;
    TAcquisitionWorker&  _acquisition_worker;
    std::mutex           _flow_graph_lock;
    flowgraph::Flowgraph _flow_graph;

public:
    using super_t = Worker<serviceName, flowgraph::FilterContext, flowgraph::Flowgraph, flowgraph::Flowgraph, Meta...>;

    explicit GnuRadioFlowGraphWorker(opencmw::URI<opencmw::STRICT> brokerAddress, const opencmw::zmq::Context& context, gr::PluginLoader* pluginLoader, flowgraph::Flowgraph initialFlowGraph, TAcquisitionWorker& acquisitionWorker, Settings settings = {}) : super_t(std::move(brokerAddress), {}, context, std::move(settings)), _plugin_loader(pluginLoader), _acquisition_worker(acquisitionWorker) { init(std::move(initialFlowGraph)); }

    template<typename BrokerType>
    explicit GnuRadioFlowGraphWorker(const BrokerType& broker, gr::PluginLoader* pluginLoader, flowgraph::Flowgraph initialFlowGraph, TAcquisitionWorker& acquisitionWorker) : super_t(broker, {}), _plugin_loader(pluginLoader), _acquisition_worker(acquisitionWorker) {
        init(std::move(initialFlowGraph));
    }

private:
    void init(flowgraph::Flowgraph initialFlowGraph) {
        super_t::setCallback([this](const RequestContext& rawCtx, const flowgraph::FilterContext& filterIn, const flowgraph::Flowgraph& in, flowgraph::FilterContext& filterOut, flowgraph::Flowgraph& out) {
            if (rawCtx.request.command == opencmw::mdp::Command::Get) {
                handleGetRequest(filterIn, filterOut, out);
            } else if (rawCtx.request.command == opencmw::mdp::Command::Set) {
                handleSetRequest(filterIn, filterOut, in, out);
            }
        });

        if (initialFlowGraph.flowgraph.empty()) {
            return;
        }

        try {
            std::lock_guard lockGuard(_flow_graph_lock);
            auto            grGraph = std::make_unique<gr::Graph>(gr::loadGrc(*_plugin_loader, initialFlowGraph.flowgraph));
            _flow_graph             = std::move(initialFlowGraph);
            _acquisition_worker.setGraph(std::move(grGraph));
        } catch (const std::string& e) {
            throw std::invalid_argument(fmt::format("Could not parse flow graph: {}", e));
        }
    }

    void handleGetRequest(const flowgraph::FilterContext& /*filterIn*/, flowgraph::FilterContext& /*filterOut*/, flowgraph::Flowgraph& out) {
        std::lock_guard lockGuard(_flow_graph_lock);
        out = _flow_graph;
    }

    void handleSetRequest(const flowgraph::FilterContext& /*filterIn*/, flowgraph::FilterContext& /*filterOut*/, const flowgraph::Flowgraph& in, flowgraph::Flowgraph& out) {
        {
            std::lock_guard lockGuard(_flow_graph_lock);
            try {
                auto grGraph = std::make_unique<gr::Graph>(gr::loadGrc(*_plugin_loader, in.flowgraph));
                _flow_graph  = in;
                out          = in;
                _acquisition_worker.setGraph(std::move(grGraph));
            } catch (const std::string& e) {
                throw std::invalid_argument(fmt::format("Could not parse flow graph: {}", e));
            }
        }
        notifyUpdate();
    }

    void notifyUpdate() {
        for (auto subTopic : super_t::activeSubscriptions()) {
            const auto           queryMap  = subTopic.params();
            const auto           filterIn  = opencmw::query::deserialise<flowgraph::FilterContext>(queryMap);
            auto                 filterOut = filterIn;
            flowgraph::Flowgraph subscriptionReply;
            handleGetRequest(filterIn, filterOut, subscriptionReply);
            super_t::notify(filterOut, subscriptionReply);
        }
    }
};

} // namespace opendigitizer::acq

#endif // OPENDIGITIZER_SERVICE_GNURADIOWORKER_H
