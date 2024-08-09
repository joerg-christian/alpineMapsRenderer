/*****************************************************************************
 * Alpine Terrain Renderer
 * Copyright (C) 2023 Adam Celarek
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

#include "nucleus/avalanche/eaws.h"
#include "nucleus/camera/Definition.h"
#include "nucleus/tile_scheduler/Cache.h"
#include "nucleus/tile_scheduler/tile_types.h"
#include "radix/tile.h"

class QTimer;

namespace nucleus {
class DataQuerier;
}

namespace nucleus::tile_scheduler {

class SchedulerEaws : public QObject {
    Q_OBJECT
public:
    struct Statistics {
        unsigned n_tiles_in_ram_cache = 0;
        unsigned n_tiles_in_gpu_cache = 0;
    };

    explicit SchedulerEaws(QObject* parent = nullptr);
    explicit SchedulerEaws(const QByteArray& default_tile, QObject* parent = nullptr); // eaws updated
    ~SchedulerEaws() override;

    void set_update_timeout(unsigned int new_update_timeout);

    [[nodiscard]] bool enabled() const;
    void set_enabled(bool new_enabled);

    void set_permissible_screen_space_error(float new_permissible_screen_space_error);

    void set_aabb_decorator(const utils::AabbDecoratorPtr& new_aabb_decorator);

    void set_gpu_quad_limit(unsigned int new_gpu_quad_limit);

    void set_ram_quad_limit(unsigned int new_ram_quad_limit);

    void set_purge_timeout(unsigned int new_purge_timeout);

    const Cache<tile_types::EawsQuad>& ram_cache() const; // eaws updated
    Cache<tile_types::EawsQuad>& ram_cache(); // eaws updated

    static QByteArray black_png_tile(unsigned size);
    static std::filesystem::path disk_cache_path();

    [[nodiscard]] unsigned int persist_timeout() const;
    void set_persist_timeout(unsigned int new_persist_timeout);

    void read_disk_cache();

    void set_retirement_age_for_tile_cache(unsigned int new_retirement_age_for_tile_cache);
    
    void set_dataquerier(std::shared_ptr<DataQuerier> dataquerier);

signals:
    void statistics_updated(Statistics stats);
    void quad_received(const tile::Id& ids);
    void quads_requested(const std::vector<tile::Id>& ids);
    void quads_without_data_requested(const std::vector<tile::Id>& ids);
    void gpu_quads_updated(const std::vector<tile_types::GpuEawsQuad>& new_quads, const std::vector<tile::Id>& deleted_quads); // eaws updated

public slots:
    void update_camera(const nucleus::camera::Definition& camera);
    void receive_quad(const tile_types::EawsQuad& new_quad); // eaws updated
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
    std::shared_ptr<DataQuerier> m_dataquerier;

private:
    unsigned m_retirement_age_for_tile_cache = 10u * 24u * 3600u * 1000u; // 10 days
    float m_permissible_screen_space_error = 2;
    unsigned m_update_timeout = 100;
    unsigned m_purge_timeout = 1000;
    unsigned m_persist_timeout = 10000;
    unsigned m_gpu_quad_limit = 300;
    unsigned m_ram_quad_limit = 15000;
    uint m_max_zoom_level = 10;
    static constexpr unsigned m_tile_size = 256; // eaws modified
    bool m_enabled = false;
    bool m_network_requests_enabled = true;
    Statistics m_statistics;
    std::unique_ptr<QTimer> m_update_timer;
    std::unique_ptr<QTimer> m_purge_timer;
    std::unique_ptr<QTimer> m_persist_timer;
    camera::Definition m_current_camera;
    utils::AabbDecoratorPtr m_aabb_decorator;
    Cache<tile_types::EawsQuad> m_ram_cache;
    Cache<tile_types::GpuCacheInfo> m_gpu_cached;
    std::shared_ptr<QByteArray> m_default_tile;
    avalanche::eaws::UIntIdManager m_eaws_uint_id_manager;
    nucleus::Raster<uint16_t> rasterize_from_max_zoom_tile(const tile::Id& input_tile_id);
};
} // namespace nucleus::tile_scheduler
