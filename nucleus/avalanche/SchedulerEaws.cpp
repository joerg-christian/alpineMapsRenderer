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

// For a given quad (!): Returns parent quad id at provided zoom level (first entry) and child of this parent quad that is parent of provided tile id
std::pair<tile::Id, tile::Id> find_parent_quad_and_child_id_at_zoom_level(const tile::Id& input_quad_id, const uint& parent_zoom_level)
{
    assert(input_quad_id.zoom_level >= parent_zoom_level);
    tile::Id parent_quad_id = input_quad_id;
    tile::Id parent_tile_id = input_quad_id;
    while (parent_quad_id.zoom_level > parent_zoom_level) {
        parent_tile_id = parent_quad_id;
        parent_quad_id = parent_quad_id.parent();
    }
    return std::make_pair(parent_quad_id, parent_tile_id);
}

nucleus::Raster<uint16_t> SchedulerEaws::rasterize_from_max_zoom_tile(const tile::Id& input_tile_id)
{
    // we only interpolate tiles above max zoom level, otherwise they are requested from eaws server!
    assert(input_tile_id.zoom_level > m_max_zoom_level);

    // get parent quad and its child containing our tile such that child tile is at max zoom level
    std::pair<tile::Id, tile::Id> parent_quad_and_child = find_parent_quad_and_child_id_at_zoom_level(input_tile_id, m_max_zoom_level - 1);
    tile::Id parent_quad_id = parent_quad_and_child.first;
    tile::Id parent_tile_id = parent_quad_and_child.second;
    assert(m_ram_cache.contains(parent_quad_id));

    // Get the parent quad at zoom level = max_zoom -1
    const tile_types::EawsQuad& parent_quad = m_ram_cache.peak_at(parent_quad_id);

    // Get parent tile at max_zoom_level
    tile_types::TileLayer parent_tile;
    for (const tile_types::TileLayer& child : parent_quad.tiles) {
        if (child.id == parent_tile_id) {
            parent_tile = child;
            break;
        }
    }

    // the parent tile having no data means it contains no regions, otherwise we use it to rasterize regions of the input tile
    if (0 == parent_tile.data->size())
        return nucleus::Raster<uint16_t>(glm::uvec2(1, 1), 0);
    auto result = avalanche::eaws::vector_tile_reader(*parent_tile.data, parent_tile_id);
    assert(result.has_value());
    return avalanche::eaws::rasterize_regions(result.value(), &m_eaws_uint_id_manager, m_tile_size, m_tile_size, input_tile_id);
}

void SchedulerEaws::update_gpu_quads()
{
    // Just visit to update expiration date
    const auto should_refine = tile_scheduler::utils::refineFunctor(m_current_camera, m_aabb_decorator, m_permissible_screen_space_error, m_tile_size);
    m_ram_cache.visit([&should_refine](const tile_types::EawsQuad& quad) {
        if (!should_refine(quad.id))
            return false;
        return true;
    });

    // List all required quads for the current camera position
    const std::vector<tile::Id> quad_ids_required_by_camera = tiles_for_current_camera_position();

    // A quad becomes gpu candidate if it is not on gpu already
    std::vector<tile::Id> gpu_candidates_ids;
    std::copy_if(
        quad_ids_required_by_camera.begin(), quad_ids_required_by_camera.end(), std::back_inserter(gpu_candidates_ids), [this](const tile::Id& quad_id) {
            if (m_gpu_cached.contains(quad_id))
                return false;
            return true;
        });

    // Only keep quads >= max zoom in gpu_candidates if they can be generated from a tile at max zoom level in the ram cache!
    std::erase_if(gpu_candidates_ids, [this](const tile::Id& quad_id) {
        if (quad_id.zoom_level >= m_max_zoom_level) {
            tile::Id parent_quad_id = find_parent_quad_and_child_id_at_zoom_level(quad_id, m_max_zoom_level - 1).first;
            if (!m_ram_cache.contains(parent_quad_id))
                return true;
        }
        return false;
    });

    // get all gpu candidates < max zoom level from the ram cache (if available there) and create dummys for those >= max zoom level
    std::vector<tile_types::EawsQuad> gpu_candidates;
    for (const tile::Id& gpu_candidate_quad_id : gpu_candidates_ids) {
        // if quad >= max zoom: register quad with no data as gpu candidate
        if (gpu_candidate_quad_id.zoom_level >= m_max_zoom_level) {
            tile_types::EawsQuad quad(gpu_candidate_quad_id);
            for (uint i = 0; i < quad.tiles.size(); i++) {
                quad.tiles[i] = tile_types::TileLayer {
                    .id = gpu_candidate_quad_id.children()[i],
                    .network_info = tile_types::NetworkInfo { tile_types::NetworkInfo::Status::Good, utils::time_since_epoch() },
                    .data = std::shared_ptr<QByteArray>(nullptr),
                };
            }
            quad.n_tiles = quad.tiles.size();
            gpu_candidates.push_back(quad);
        } else if (m_ram_cache.contains(gpu_candidate_quad_id)) // below max zoom eaws tiles are stored in ram cache
            gpu_candidates.push_back(m_ram_cache.peak_at(gpu_candidate_quad_id));
    }

    // Register all ids that will be pushed to gpu as being in gpu cache
    for (const auto& q : gpu_candidates) {
        m_gpu_cached.insert(tile_types::GpuCacheInfo { q.id });
    }
    m_gpu_cached.visit([&should_refine](const tile_types::GpuCacheInfo& quad) { return should_refine(quad.id); });

    // elimitate double entries (happens when the gpu has not enough space for all quads selected above)
    const auto superfluous_quads = m_gpu_cached.purge(m_gpu_quad_limit);
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

    // Create rasters for new gpu quads to be stored and used on gpu
    std::vector<tile_types::GpuEawsQuad> new_gpu_quads;
    new_gpu_quads.reserve(gpu_candidates_ids.size());
    std::transform(gpu_candidates.cbegin(), gpu_candidates.cend(), std::back_inserter(new_gpu_quads), [this](const tile_types::EawsQuad& cpu_quad) {
        // create GpuQuad based on cpu quad or create it if it is >= zoom level
        tile_types::GpuEawsQuad gpu_quad;
        gpu_quad.id = cpu_quad.id;
        assert(cpu_quad.n_tiles == 4);
        for (unsigned i = 0; i < 4; ++i) {
            gpu_quad.tiles[i].id = cpu_quad.tiles[i].id;
            gpu_quad.tiles[i].bounds = m_aabb_decorator->aabb(cpu_quad.tiles[i].id);
            nucleus::Raster<uint16_t> raster(glm::uvec2(m_tile_size, m_tile_size), 0);
            if (cpu_quad.tiles[i].id.zoom_level > m_max_zoom_level)
                raster = rasterize_from_max_zoom_tile(cpu_quad.tiles[i].id);
            else if (cpu_quad.tiles[i].data->size() > 0) {
                const auto* eaws_data = cpu_quad.tiles[i].data.get();
                tl::expected<avalanche::eaws::RegionTile, QString> result = avalanche::eaws::vector_tile_reader(*eaws_data, cpu_quad.tiles[i].id);
                assert(result.has_value());
                avalanche::eaws::RegionTile region_tile = result.value();
                raster = avalanche::eaws::rasterize_regions(region_tile, &m_eaws_uint_id_manager, m_tile_size, m_tile_size, cpu_quad.tiles[i].id);
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

    // Remove tiles that are already in cache and not expired
    const auto current_time = utils::time_since_epoch();
    std::erase_if(currently_active_tiles, [this, current_time](const tile::Id& id) {
        return m_ram_cache.contains(id) && m_ram_cache.peak_at(id).network_info().timestamp + m_retirement_age_for_tile_cache > current_time;
    });

    // Check which tiles have a zoom level higher than what the server can provide and send those to be interpolated from existing tiles
    std::vector<tile::Id> fetch_these_quads_from_server;
    std::for_each(currently_active_tiles.begin(), currently_active_tiles.end(), [this, current_time, &fetch_these_quads_from_server](const tile::Id& id) {
        if (id.zoom_level < m_max_zoom_level // child tiles of quad can be served by tile server
            && (!m_ram_cache.contains(id) || m_ram_cache.peak_at(id).network_info().timestamp + m_retirement_age_for_tile_cache <= current_time))
            fetch_these_quads_from_server.push_back(id);
        else {
            tile::Id parent_quad_id = find_parent_quad_and_child_id_at_zoom_level(id, m_max_zoom_level - 1).first;
            if (!m_ram_cache.contains(parent_quad_id)) {
                fetch_these_quads_from_server.push_back(parent_quad_id);
            }
        }
    });
    emit quads_requested(fetch_these_quads_from_server);
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
    const auto all_leaves = quad_tree::onTheFlyTraverse(tile::Id { 0, { 0, 0 } },
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
