#ifndef DASHBOARD_H
#define DASHBOARD_H

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef EMSCRIPTEN
#include "emscripten_compat.h"
#endif
#include <plf_colony.h>

namespace DigitizerUi {

class Block;
class FlowGraph;
struct DashboardDescription;

struct DashboardSource {
    std::string                           path;
    bool                                  enabled;
    const bool                            isValid = true;

    std::shared_ptr<DashboardDescription> load(const std::string &filename);
};

struct DashboardDescription {
    static constexpr const char                                      *fileExtension = ".ddd"; // ddd for "Digitizer Dashboard Description"

    std::string                                                       name;
    DashboardSource                                                  *source;
    bool                                                              isFavorite;
    std::optional<std::chrono::time_point<std::chrono::system_clock>> lastUsed;

    void                                                              save();

    static std::shared_ptr<DashboardDescription>                      createEmpty(const std::string &name);
};

class Dashboard {
public:
    struct Source {
        Block      *block;
        int         port;
        std::string name;
        uint32_t    color;

        inline bool operator==(const Source &s) const { return s.block == block && s.port == port; };
    };
    struct Plot {
        Plot();

        std::string           name;
        std::vector<Source *> sources;
    };

    explicit Dashboard(const std::shared_ptr<DashboardDescription> &desc, FlowGraph *fg);
    ~Dashboard();

    void               save();

    inline const auto &sources() const { return m_sources; }
    inline auto       &sources() { return m_sources; }

    inline auto       &plots() { return m_plots; }

    void                         setNewDescription(const std::shared_ptr<DashboardDescription> &desc);
    inline DashboardDescription *description() const { return m_desc.get(); }

private:
    std::shared_ptr<DashboardDescription> m_desc;
    FlowGraph        *m_flowGraph;
    std::vector<Plot> m_plots;
    plf::colony<Source>                   m_sources;
};

} // namespace DigitizerUi

#endif
