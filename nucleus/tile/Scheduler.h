/*****************************************************************************
 * AlpineMaps.org
 * Copyright (C) 2023 Adam Celarek
 * Copyright (C) 2024 Gerald Kimmersdorfer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *****************************************************************************/

#pragma once

#include <memory>

#include <QNetworkInformation>
#include <QObject>
#include "Cache.h"
#include "nucleus/camera/Definition.h"
#include "radix/tile.h"
#include "types.h"

class QTimer;

namespace nucleus {
class DataQuerier;
}

namespace nucleus::tile {
namespace utils {
    class AabbDecorator;
    using AabbDecoratorPtr = std::shared_ptr<AabbDecorator>;
}

class Scheduler : public QObject {
    Q_OBJECT
public:
    struct Statistics {
        unsigned n_tiles_in_ram_cache = 0;
        unsigned n_tiles_in_gpu_cache = 0;
    };

    explicit Scheduler(std::string name, unsigned tile_resolution = 256, QObject* parent = nullptr);
    // Seconds constructor still here for tests, Is it necessary?
    ~Scheduler() override;

    void set_update_timeout(unsigned int new_update_timeout);

    [[nodiscard]] bool enabled() const;
    void set_enabled(bool new_enabled);

    void set_permissible_screen_space_error(float new_permissible_screen_space_error);

    void set_aabb_decorator(const utils::AabbDecoratorPtr& new_aabb_decorator);

    void set_gpu_quad_limit(unsigned int new_gpu_quad_limit);

    void set_ram_quad_limit(unsigned int new_ram_quad_limit);

    void set_purge_timeout(unsigned int new_purge_timeout);

    const Cache<DataQuad>& ram_cache() const;
    Cache<DataQuad>& ram_cache();

    std::filesystem::path disk_cache_path();

    [[nodiscard]] unsigned int persist_timeout() const;
    void set_persist_timeout(unsigned int new_persist_timeout);

    void read_disk_cache();

    void set_retirement_age_for_tile_cache(unsigned int new_retirement_age_for_tile_cache);
    
    void set_dataquerier(std::shared_ptr<DataQuerier> dataquerier);
    std::shared_ptr<DataQuerier> dataquerier() const;

    [[nodiscard]] unsigned int tile_resolution() const;

    const utils::AabbDecoratorPtr& aabb_decorator() const;

signals:
    void statistics_updated(Statistics stats);
    void quad_received(const tile::Id& ids);
    void quads_requested(const std::vector<tile::Id>& ids);

public slots:
    void update_camera(const nucleus::camera::Definition& camera);
    void receive_quad(const DataQuad& new_quad);
    void set_network_reachability(QNetworkInformation::Reachability reachability);
    void update_gpu_quads();
    void send_quad_requests();
    void purge_ram_cache();
    void persist_tiles();

protected:
    void schedule_update();
    void schedule_purge();
    void schedule_persist();
    void update_stats();
    std::vector<tile::Id> tiles_for_current_camera_position() const;
    virtual bool is_ready_to_ship(const DataQuad&) const { return true; }
    virtual void transform_and_emit(const std::vector<DataQuad>& new_quads, const std::vector<tile::Id>& deleted_quads) = 0;
    uint m_max_tile_zoom_level = 18;

private:
    std::string m_name = "";
    std::shared_ptr<DataQuerier> m_dataquerier;
    unsigned m_retirement_age_for_tile_cache = 10u * 24u * 3600u * 1000u; // 10 days
    float m_permissible_screen_space_error = 2;
    unsigned m_update_timeout = 100;
    unsigned m_purge_timeout = 1000;
    unsigned m_persist_timeout = 10000;
    unsigned m_gpu_quad_limit = 300;
    unsigned m_ram_quad_limit = 15000;
    unsigned m_tile_resolution = 256;
    bool m_enabled = false;
    bool m_network_requests_enabled = true;
    Statistics m_statistics;
    std::unique_ptr<QTimer> m_update_timer;
    std::unique_ptr<QTimer> m_purge_timer;
    std::unique_ptr<QTimer> m_persist_timer;
    camera::Definition m_current_camera;
    utils::AabbDecoratorPtr m_aabb_decorator;
    Cache<DataQuad> m_ram_cache;
    Cache<GpuCacheInfo> m_gpu_cached;
};
}
