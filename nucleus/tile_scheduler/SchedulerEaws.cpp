/*****************************************************************************
 * Alpine Terrain Renderer
 * Copyright (C) 2023 Adam Celarek
 * Copyright (C) 2024 Lucas Dworschak
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

#include "SchedulerEaws.h"
#include <unordered_set>

#include <QBuffer>
#include <QDebug>
#include <QNetworkInformation>
#include <QStandardPaths>
#include <QTimer>

#include "nucleus/tile_scheduler/utils.h"
#include "nucleus/vector_tiles/VectorTileManager.h"
#include "radix/quad_tree.h"

#include "nucleus/DataQuerier.h"

using namespace nucleus::tile_scheduler;

SchedulerEaws::SchedulerEaws(QObject* parent)
    : SchedulerEaws { black_png_tile(m_tile_size), parent }
{
}

SchedulerEaws::SchedulerEaws(const QByteArray& default_tile, QObject* parent)
    : QObject { parent }
{
    m_update_timer = std::make_unique<QTimer>(this);
    m_update_timer->setSingleShot(true);
    connect(m_update_timer.get(), &QTimer::timeout, this, &SchedulerEaws::send_quad_requests);
    connect(m_update_timer.get(), &QTimer::timeout, this, &SchedulerEaws::update_gpu_quads);

    m_purge_timer = std::make_unique<QTimer>(this);
    m_purge_timer->setSingleShot(true);
    connect(m_purge_timer.get(), &QTimer::timeout, this, &SchedulerEaws::purge_ram_cache);

    m_persist_timer = std::make_unique<QTimer>(this);
    m_persist_timer->setSingleShot(true);
    connect(m_persist_timer.get(), &QTimer::timeout, this, &SchedulerEaws::persist_tiles);

    m_default_tile = std::make_shared<QByteArray>(default_tile);
    connect(this, &SchedulerEaws::quad_above_zoom_level_created, this, &SchedulerEaws::receive_quad);
    connect(this, &SchedulerEaws::quads_without_data_requested, this, &SchedulerEaws::create_quads_without_data);
}

SchedulerEaws::~SchedulerEaws() = default;

void SchedulerEaws::update_camera(const camera::Definition& camera)
{
    m_current_camera = camera;
    schedule_update();
}

void SchedulerEaws::receive_quad(const tile_types::EawsQuad& new_quad)
{
    using Status = tile_types::NetworkInfo::Status;
#ifdef __EMSCRIPTEN__
    // webassembly doesn't report 404 (well, probably it does, but not if there is a cors failure as well).
    // so we'll simply treat any 404 as network error.
    // however, we need to pass tiles with zoomlevel < 10, otherwise the top of the tree won't be built.
    if (new_quad.network_info().status == Status::Good || new_quad.id.zoom_level < 10) {
        m_ram_cache.insert(new_quad);
        schedule_purge();
        schedule_update();
        schedule_persist();
        emit quad_received(new_quad.id);
    }
#else
    switch (new_quad.network_info().status) {
    case Status::Good:
    case Status::NotFound:
        if (new_quad.id.zoom_level == 0)
            std::cout << "!!";
        if (new_quad.id.zoom_level < 11)
            std::cout << "!!";
        assert(new_quad.n_tiles == 4);
        m_ram_cache.insert(new_quad);
        schedule_purge();
        schedule_update();
        schedule_persist();
        emit quad_received(new_quad.id);
        update_stats();
        break;
    case Status::NetworkError:
        // do not persist the tile.
        // do not reschedule retrieval (wait for user input or a reconnect signal).
        // do not purge (nothing was added, so no need to check).
        // do nothing.
        break;
    }
#endif
}

void SchedulerEaws::set_network_reachability(QNetworkInformation::Reachability reachability)
{
    switch (reachability) {
    case QNetworkInformation::Reachability::Online:
    case QNetworkInformation::Reachability::Local:
    case QNetworkInformation::Reachability::Site:
    case QNetworkInformation::Reachability::Unknown:
        qDebug() << "enabling network";
        m_network_requests_enabled = true;
        schedule_update();
        break;
    case QNetworkInformation::Reachability::Disconnected:
        qDebug() << "disabling network";
        m_network_requests_enabled = false;
        break;
    }
}

nucleus::Raster<uint16_t> SchedulerEaws::interpolate_tile(const tile::Id& input_tile_id)
{
    // we only interpolate tiles above max zoom level, otherwise they are requested from eaws server!
    assert(input_tile_id.zoom_level > m_max_zoom_level);

    // Calculate parent quad id on max zoom level
    tile::Id best_parent_quad_id = input_tile_id;
    while (best_parent_quad_id.zoom_level > m_max_zoom_level) {
        best_parent_quad_id = best_parent_quad_id.parent();
    }

    // Go through parent quads starting at max zoom level -1 and check if they are in chache
    // For this to work at least quad for zoom = 0 must be in cache
    tile::Id best_parent_tile_id;
    while (best_parent_quad_id.zoom_level > 0) {
        best_parent_tile_id = best_parent_quad_id;
        best_parent_quad_id = best_parent_quad_id.parent();
        if (best_parent_quad_id.zoom_level == 0)
            assert(m_ram_cache.contains(best_parent_quad_id));
        //        if (m_ram_cache.contains(best_parent_quad_id))
        //            break;
    }

    // Get the best (= highest zoom) available quad
    const tile_types::EawsQuad& best_parent_quad = m_ram_cache.peak_at(best_parent_quad_id);

    // Get child tile that contains the input tile
    tile_types::TileLayer best_parent_tile;
    for (const tile_types::TileLayer& child : best_parent_quad.tiles) {
        if (child.id == best_parent_tile_id) {
            best_parent_tile = child;
            break;
        }
    }

    // the parent tile of zoom lvel < 10 having no data means it contains no regions
    if (0 == best_parent_tile.data->size())
        return nucleus::Raster<uint16_t>(glm::uvec2(1, 1), 0);
    auto result = avalanche::eaws::vector_tile_reader(*best_parent_tile.data, best_parent_tile_id);
    return avalanche::eaws::rasterize_regions(result.value(), &m_eaws_uint_id_manager, m_tile_size, m_tile_size, input_tile_id);
}

void SchedulerEaws::update_gpu_quads()
{
    const auto should_refine = tile_scheduler::utils::refineFunctor(m_current_camera, m_aabb_decorator, m_permissible_screen_space_error, m_tile_size);
    m_ram_cache.visit([&should_refine](const tile_types::EawsQuad& quad) {
        if (!should_refine(quad.id))
            return false;
        return true;
    });
    const std::vector<tile::Id> all_quads = tiles_for_current_camera_position();
    std::vector<tile::Id> generatable_quads;
    std::copy_if(all_quads.begin(), all_quads.end(), std::back_inserter(generatable_quads), [](const tile::Id& tile_id) {
        /// TODO: return true if generatable
        return false;
    });

    std::vector<tile::Id> gpu_candidates;
    std::copy_if(generatable_quads.begin(), generatable_quads.end(), std::back_inserter(gpu_candidates), [this](const tile::Id& tile_id) {
        if (m_gpu_cached.contains(tile_id))
            return false;
        return true;
    });

    for (const auto& q : gpu_candidates) {
        m_gpu_cached.insert(tile_types::GpuCacheInfo { q });
    }

    m_gpu_cached.visit([&should_refine](const tile_types::GpuCacheInfo& quad) {
        return should_refine(quad.id);
    });

    const auto superfluous_quads = m_gpu_cached.purge(m_gpu_quad_limit);

    // elimitate double entries (happens when the gpu has not enough space for all quads selected above)
    std::unordered_set<tile::Id, tile::Id::Hasher> superfluous_ids;
    superfluous_ids.reserve(superfluous_quads.size());
    for (const auto& quad : superfluous_quads)
        superfluous_ids.insert(quad.id);

    std::erase_if(gpu_candidates, [&superfluous_ids](const auto& quad) {
        if (superfluous_ids.contains(quad.id)) {
            superfluous_ids.erase(quad.id);
            return true;
        }
        return false;
    });

    std::vector<tile_types::GpuEawsQuad> new_gpu_quads;
    new_gpu_quads.reserve(gpu_candidates.size());
    std::transform(gpu_candidates.cbegin(), gpu_candidates.cend(), std::back_inserter(new_gpu_quads), [this](const tile_types::EawsQuad& quad) {
        // create GpuQuad based on cpu quad
        tile_types::GpuEawsQuad gpu_quad;
        gpu_quad.id = quad.id;
        assert(quad.n_tiles == 4);
        for (unsigned i = 0; i < 4; ++i) {
            gpu_quad.tiles[i].id = quad.tiles[i].id;
            nucleus::Raster<uint16_t> raster(glm::uvec2(m_tile_size, m_tile_size), 0);
            if (quad.tiles[i].id.zoom_level > m_max_zoom_level)
                raster = interpolate_tile(quad.tiles[i].id);
            else if (quad.tiles[i].data->size() > 0) {
                const auto* eaws_data = quad.tiles[i].data.get();
                tl::expected<avalanche::eaws::RegionTile, QString> result = avalanche::eaws::vector_tile_reader(*eaws_data, quad.tiles[i].id);
                raster = avalanche::eaws::rasterize_regions(result.value(), &m_eaws_uint_id_manager, m_tile_size, m_tile_size, quad.tiles[i].id);
            }
            gpu_quad.tiles[i].raster = std::make_shared<nucleus::Raster<uint16_t>>(std::move(raster));
        }
        return gpu_quad;
    });

    emit gpu_quads_updated(new_gpu_quads, { superfluous_ids.cbegin(), superfluous_ids.cend() });
    update_stats();
}

void SchedulerEaws::send_quad_requests()
{
    if (!m_network_requests_enabled)
        return;
    auto currently_active_tiles = tiles_for_current_camera_position();

    // Remove tiles that  already in cache and not expired
    const auto current_time = utils::time_since_epoch();
    std::erase_if(currently_active_tiles, [this, current_time](const tile::Id& id) {
        return m_ram_cache.contains(id) && m_ram_cache.peak_at(id).network_info().timestamp + m_retirement_age_for_tile_cache > current_time;
    });

    // Check which tiles have a zoom level higher than what the server can provide and send those to be interpolated from existing tiles
    std::vector<tile::Id> fetch_these_quads_from_server;
    std::vector<tile::Id> interpolate_these;

    std::for_each(currently_active_tiles.begin(), currently_active_tiles.end(),
        [this, current_time, &fetch_these_quads_from_server, &interpolate_these](const tile::Id& id) {
            if (!m_ram_cache.contains(id) || m_ram_cache.peak_at(id).network_info().timestamp + m_retirement_age_for_tile_cache <= current_time) {
                // this tile is not available in the cache, decide how to generate it:
                if (id.zoom_level > m_max_zoom_level)
                    interpolate_these.push_back(id);
                else
                    fetch_these_quads_from_server.push_back(id);
            }
        });
    emit quads_requested(fetch_these_quads_from_server);
    // emit quads_without_data_requested(interpolate_these);
}

void SchedulerEaws::create_quads_without_data(const std::vector<tile::Id>& input_tile_ids)
{
    // Only create empty quads above maximum zoom level.
    // These will be substituted by an interpolation later when gpu quads are built, thus they get nullptr as data attribute
    // thus we have to wait until at least tile 0 is in Cache
    if (m_ram_cache.contains(tile::Id(0, glm::uvec2(0, 0), tile::Scheme::SlippyMap))) {
        for (const tile::Id& quad_id : input_tile_ids) {
            assert(quad_id.zoom_level > m_max_zoom_level);
            tile_types::EawsQuad new_eaws_quad;
            new_eaws_quad.n_tiles = 4;
            new_eaws_quad.id = quad_id;
            tile_types::NetworkInfo network_info({ tile_types::NetworkInfo::Status::Good, utils::time_since_epoch() });
            for (uint i = 0; i < 4; i++) {
                new_eaws_quad.tiles[i] = tile_types::TileLayer({ quad_id.children()[i], network_info, std::shared_ptr<QByteArray>(nullptr) });
            }
            emit quad_above_zoom_level_created(new_eaws_quad);
        }
    }
}

void SchedulerEaws::purge_ram_cache()
{
    if (m_ram_cache.n_cached_objects() <= unsigned(float(m_ram_quad_limit) * 1.05f)){
        return;
    }

    const auto should_refine = tile_scheduler::utils::refineFunctor(m_current_camera, m_aabb_decorator, m_permissible_screen_space_error, m_tile_size);
    m_ram_cache.visit([&should_refine](const tile_types::EawsQuad& quad) { return should_refine(quad.id); });
    m_ram_cache.purge(m_ram_quad_limit);
    update_stats();
}

void SchedulerEaws::persist_tiles()
{
    const auto start = std::chrono::steady_clock::now();
    const auto r = m_ram_cache.write_to_disk(disk_cache_path());
    const auto diff = std::chrono::steady_clock::now() - start;

    if (diff > std::chrono::milliseconds(50))
        qDebug() << QString("Scheduler::persist_tiles took %1ms for %2 quads.")
                        .arg(std::chrono::duration_cast<std::chrono::milliseconds>(diff).count())
                        .arg(m_ram_cache.n_cached_objects());

    if (!r.has_value()) {
        qDebug() << QString("Writing tiles to disk into %1 failed: %2. Removing all files.")
                        .arg(QString::fromStdString(disk_cache_path().string()))
                        .arg(QString::fromStdString(r.error()));
        std::filesystem::remove_all(disk_cache_path());
    }
}

void SchedulerEaws::schedule_update()
{
    assert(m_update_timeout < unsigned(std::numeric_limits<int>::max()));
    if (m_enabled && !m_update_timer->isActive())
        m_update_timer->start(int(m_update_timeout));
}

void SchedulerEaws::schedule_purge()
{
    assert(m_purge_timeout < unsigned(std::numeric_limits<int>::max()));
    if (m_enabled && !m_purge_timer->isActive()) {
        m_purge_timer->start(int(m_purge_timeout));
    }
}

void SchedulerEaws::schedule_persist()
{
    assert(m_persist_timeout < unsigned(std::numeric_limits<int>::max()));
    if (!m_persist_timer->isActive()) {
        m_persist_timer->start(int(m_persist_timeout));
    }
}

void SchedulerEaws::update_stats()
{
    m_statistics.n_tiles_in_ram_cache = m_ram_cache.n_cached_objects();
    m_statistics.n_tiles_in_gpu_cache = m_gpu_cached.n_cached_objects();
    emit statistics_updated(m_statistics);
}

void SchedulerEaws::read_disk_cache()
{
    const auto r = m_ram_cache.read_from_disk(disk_cache_path());
    if (r.has_value()) {
        update_stats();
    } else {
        qDebug() << QString("Reading tiles from disk cache (%1) failed: \n%2\nRemoving all files.")
                        .arg(QString::fromStdString(disk_cache_path().string()))
                        .arg(QString::fromStdString(r.error()));
        std::filesystem::remove_all(disk_cache_path());
    }
}

std::vector<tile::Id> SchedulerEaws::tiles_for_current_camera_position() const
{
    std::vector<tile::Id> all_inner_nodes;
    const auto all_leaves = quad_tree::onTheFlyTraverse(tile::Id { 0, { 0, 0 }, tile::Scheme::SlippyMap },
        tile_scheduler::utils::refineFunctor(m_current_camera, m_aabb_decorator, m_permissible_screen_space_error, m_tile_size),
        [&all_inner_nodes](const tile::Id& v) {
            all_inner_nodes.push_back(v);
            return v.children();
        });

    // not adding leaves, because they we will be fetching quads, which also fetch their children
    return all_inner_nodes;
}

void SchedulerEaws::set_retirement_age_for_tile_cache(unsigned int new_retirement_age_for_tile_cache)
{
    m_retirement_age_for_tile_cache = new_retirement_age_for_tile_cache;
}

unsigned int SchedulerEaws::persist_timeout() const { return m_persist_timeout; }

void SchedulerEaws::set_persist_timeout(unsigned int new_persist_timeout)
{
    assert(new_persist_timeout < unsigned(std::numeric_limits<int>::max()));
    m_persist_timeout = new_persist_timeout;

    if (m_persist_timer->isActive()) {
        m_persist_timer->start(int(m_persist_timeout));
    }
}

const Cache<tile_types::EawsQuad>& SchedulerEaws::ram_cache() const { return m_ram_cache; }

Cache<tile_types::EawsQuad>& SchedulerEaws::ram_cache() { return m_ram_cache; }

QByteArray SchedulerEaws::black_png_tile(unsigned size)
{
    QImage default_tile(QSize { int(size), int(size) }, QImage::Format_ARGB32);
    default_tile.fill(Qt::GlobalColor::black);
    QByteArray arr;
    QBuffer buffer(&arr);
    buffer.open(QIODevice::WriteOnly);
    default_tile.save(&buffer, "PNG");
    return arr;
}

std::filesystem::path SchedulerEaws::disk_cache_path()
{
    const auto base_path = std::filesystem::path(QStandardPaths::writableLocation(QStandardPaths::CacheLocation).toStdString());
    std::filesystem::create_directories(base_path);
    return base_path / "eaws_tile_cache";
}

void SchedulerEaws::set_purge_timeout(unsigned int new_purge_timeout)
{
    assert(new_purge_timeout < unsigned(std::numeric_limits<int>::max()));
    m_purge_timeout = new_purge_timeout;

    if (m_purge_timer->isActive()) {
        m_purge_timer->start(int(m_update_timeout));
    }
}

void SchedulerEaws::set_ram_quad_limit(unsigned int new_ram_quad_limit) { m_ram_quad_limit = new_ram_quad_limit; }

void SchedulerEaws::set_gpu_quad_limit(unsigned int new_gpu_quad_limit) { m_gpu_quad_limit = new_gpu_quad_limit; }

void SchedulerEaws::set_aabb_decorator(const utils::AabbDecoratorPtr& new_aabb_decorator) { m_aabb_decorator = new_aabb_decorator; }

void SchedulerEaws::set_permissible_screen_space_error(float new_permissible_screen_space_error)
{
    m_permissible_screen_space_error = new_permissible_screen_space_error;
}

bool SchedulerEaws::enabled() const { return m_enabled; }

void SchedulerEaws::set_enabled(bool new_enabled)
{
    m_enabled = new_enabled;
    schedule_update();
}

void SchedulerEaws::set_update_timeout(unsigned new_update_timeout)
{
    assert(m_update_timeout < unsigned(std::numeric_limits<int>::max()));
    m_update_timeout = new_update_timeout;
    if (m_update_timer->isActive()) {
        m_update_timer->start(m_update_timeout);
    }
}

void SchedulerEaws::set_dataquerier(std::shared_ptr<DataQuerier> dataquerier) { m_dataquerier = dataquerier; }
