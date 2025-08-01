/*****************************************************************************
 * AlpineMaps.org
 * Copyright (C) 2024 Adam Celarek
 * Copyright (C) 2024 Jörg Christian Reiher
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

#include <QFile>
#include <QSignalSpy>
#include <catch2/catch_test_macros.hpp>
#include <extern/radix/src/radix/tile.h>
#include <nucleus/avalanche/ReportLoadService.h>
#include <nucleus/avalanche/UIntIdManager.h>
#include <nucleus/avalanche/eaws.h>

TEST_CASE("nucleus/EAWS Vector Tiles")
{
    // Check if test file can be loaded
    const std::string test_file_name = "eaws_0-0-0.mvt";
    QString filepath = QString("%1%2").arg(ALP_TEST_DATA_DIR, test_file_name.c_str());
    QFile test_file(filepath);
    CHECK(test_file.exists());
    CHECK(test_file.size() > 0);

    // Check if testfile can be opened and read
    test_file.open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    QByteArray test_data = test_file.readAll();
    test_file.close();
    CHECK(test_data.size() > 0);

    // Check if testdata can be converted to string
    std::string test_data_as_string = test_data.toStdString();
    CHECK(test_data_as_string.size() > 0);

    // Check if tile loading works and at least one layer is present
    mapbox::vector_tile::buffer tileBuffer(test_data_as_string);
    std::map<std::string, const protozero::data_view> layers = tileBuffer.getLayers();
    CHECK(layers.size() > 0);

    // Check if layer with name "micro-regions" exists
    CHECK(layers.contains("micro-regions"));
    mapbox::vector_tile::layer layer = tileBuffer.getLayer("micro-regions");

    // Check if layer micro-regions has enough regions
    CHECK(layer.featureCount() > 0);

    // Check if extend (tile resolution) is >0
    CHECK(layer.getExtent() > 0);

    // Check if reader returns a std::vector with EAWS regions when reading mvt file
    radix::tile::Id tile_id_0_0_0({ 0, glm::uvec2(0, 0), radix::tile::Scheme::SlippyMap });
    tl::expected<nucleus::avalanche::RegionTile, QString> result = nucleus::avalanche::vector_tile_reader(test_data, tile_id_0_0_0);
    CHECK(result.has_value());

    // Check if EAWS region struct is initialized with empty attributes
    const nucleus::avalanche::Region empty_eaws_region;
    CHECK("" == empty_eaws_region.id);
    CHECK(std::nullopt == empty_eaws_region.id_alt);
    CHECK(std::nullopt == empty_eaws_region.start_date);
    CHECK(std::nullopt == empty_eaws_region.end_date);
    CHECK(empty_eaws_region.vertices_in_local_coordinates.empty());

    // Check for some samples of the returned regions if they have the correct properties
    nucleus::avalanche::RegionTile region_tile_0_0_0;
    std::vector<nucleus::avalanche::Region> eaws_regions_0_0_0;
    if (result.has_value()) {
        // Retrieve vector of all eaws regions
        region_tile_0_0_0 = result.value();
        eaws_regions_0_0_0 = region_tile_0_0_0.second;

        // Retrieve samples that should have certain properties
        nucleus::avalanche::Region region_with_start_date, region_with_end_date, region_with_id_alt;
        for (const nucleus::avalanche::Region& region : eaws_regions_0_0_0) {
            if ("DE-BY-10" == region.id) {
                region_with_id_alt = region;
                region_with_end_date = region;
            }

            if ("IT-23-AO-22" == region.id)
                region_with_start_date = region;

            if (region_with_start_date.id != "" && region_with_end_date.id != "" && region_with_id_alt.id != "")
                break;
        }

        // Check if sample has correct id
        CHECK("DE-BY-10" == region_with_id_alt.id);

        // Check if sample has correct id_alt
        CHECK("BYALL" == region_with_id_alt.id_alt);

        // Check if sample has correct start date
        CHECK(QDate::fromString(QString("2022-03-04"), "yyyy-MM-dd") == region_with_start_date.start_date);

        // Check if struct regions have correct end date for a sample
        CHECK(QDate::fromString(QString("2021-10-01"), "yyyy-MM-dd") == region_with_end_date.end_date);

        // Check if struct regions have correct boundary vertices for a sample
        std::vector<glm::vec2> vertices = region_with_start_date.vertices_in_local_coordinates;
        glm::uvec2 extent(region_with_start_date.resolution.x, region_with_start_date.resolution.y);
        CHECK(4 == vertices.size());
        if (4 == vertices.size()) {
            CHECK(2128 == (int)(vertices[0].x * extent.x));
            CHECK(1459 == (int)(vertices[0].y * extent.y));
            CHECK(2128 == (int)(vertices[1].x * extent.x));
            CHECK(1461 == (int)(vertices[1].y * extent.y));
            CHECK(2128 == (int)(vertices[3].x * extent.x));
            CHECK(1459 == (int)(vertices[3].y * extent.y));
        }

        // Create internal id manager that is later needed to write region ids to image pixels
        nucleus::avalanche::UIntIdManager internal_id_manager;
        CHECK(internal_id_manager.convert_region_id_to_internal_id("") == 0);
        std::vector<QString> all_region_Ids = internal_id_manager.get_all_registered_region_ids();
        bool internal_maps_match = true;
        for (uint i = 0; i < all_region_Ids.size(); i++) {
            if (internal_id_manager.convert_region_id_to_internal_id(all_region_Ids[i]) != i) {
                internal_maps_match = false;
                break;
            }
        }
        CHECK(internal_maps_match);

        // Check if conversion color << id << color works consistentenly
        bool wrong_conversion = false;
        for (QString region_id : all_region_Ids) {
            QColor color = internal_id_manager.convert_region_id_to_color(region_id);
            QString region_id_from_color = internal_id_manager.convert_color_to_region_id(color, QImage::Format_ARGB32);
            wrong_conversion = (region_id != region_id_from_color);
            if (wrong_conversion)
                break;
        }
        CHECK((!wrong_conversion));

        // Load tiles at higher zoom level for testing
        std::vector<std::string> file_names({ "eaws_2-2-0.mvt", "eaws_10-236-299.mvt" });
        radix::tile::Id tile_id_2_2_0 = radix::tile::Id(radix::tile::Id(2, glm::vec2(2, 0), radix::tile::Scheme::SlippyMap));
        radix::tile::Id tile_id_10_236_299 = radix::tile::Id(radix::tile::Id(10, glm::vec2(236, 299), radix::tile::Scheme::SlippyMap));
        std::vector<radix::tile::Id> tile_ids_at_zoom_Level_2({ tile_id_2_2_0, tile_id_10_236_299 });
        std::vector<nucleus::avalanche::RegionTile> region_tiles_at_zoom_level_2;
        for (uint i = 0; i < file_names.size(); i++) {
            std::string test_file_name2 = file_names[i];
            filepath = QString("%1%2").arg(ALP_TEST_DATA_DIR, test_file_name2.c_str());
            QFile test_file2(filepath);
            CHECK(test_file2.exists());
            test_file2.open(QIODevice::ReadOnly | QIODevice::Unbuffered);
            QByteArray test_data2 = test_file2.readAll();
            test_file2.close();
            CHECK(test_data2.size() > 0);
            std::string test_data2_as_string = test_data2.toStdString();
            mapbox::vector_tile::buffer tileBuffer2(test_data2_as_string);
            layers = tileBuffer2.getLayers();
            CHECK(layers.contains("micro-regions"));
            layer = tileBuffer2.getLayer("micro-regions");
            CHECK(layer.featureCount() > 0);
            CHECK(layer.getExtent() > 0);
            auto result = nucleus::avalanche::vector_tile_reader(test_data2, tile_ids_at_zoom_Level_2[i]);
            CHECK(result.has_value());
            if (result.has_value())
                region_tiles_at_zoom_level_2.push_back(result.value());
        }
        CHECK(region_tiles_at_zoom_level_2.size() == file_names.size());
        nucleus::avalanche::RegionTile region_tile_2_2_0;
        nucleus::avalanche::RegionTile region_tile_10_236_299;
        if (2 <= region_tiles_at_zoom_level_2.size()) {
            region_tile_2_2_0 = region_tiles_at_zoom_level_2[0];
            region_tile_10_236_299 = region_tiles_at_zoom_level_2[1];
        }

        // Rasterize all regions at same raster reslution as input regions
        const auto raster = nucleus::avalanche::rasterize_regions(
            region_tile_0_0_0, internal_id_manager, region_with_start_date.resolution.x, region_with_start_date.resolution.y, tile_id_0_0_0);

        // Check if raster has correct size
        CHECK((raster.width() == region_with_start_date.resolution.x && raster.height() == region_with_start_date.resolution.y));

        // Check if raster contains correct internal region-ids at certain pixels
        CHECK(0 == raster.pixel(glm::uvec2(0, 0)));
        CHECK(internal_id_manager.convert_region_id_to_internal_id(region_with_start_date.id) == raster.pixel(glm::vec2(2128, 1459)));

        // Check if raster and image have same values when drawn with same resolution
        QImage img_small = nucleus::avalanche::draw_regions(region_tile_2_2_0, internal_id_manager, 20, 20, tile_id_2_2_0);
        const auto raster_small = nucleus::avalanche::rasterize_regions(region_tile_2_2_0, internal_id_manager, 20, 20, tile_id_2_2_0);
        for (uint i = 0; i < 10; i++) {
            for (uint j = 0; j < 10; j++) {
                uint id_from_img = internal_id_manager.convert_color_to_internal_id(img_small.pixel(i, j), QImage::Format_ARGB32);
                uint id_from_raster = raster_small.pixel(glm::uvec2(i, j));
                CHECK(id_from_img == id_from_raster);
            }
        }

        // Check if tile that has only region NO-3035 in it produces a 1x1 raster with the corresponding internal region id
        const auto raster_with_one_pixel = nucleus::avalanche::rasterize_regions(region_tile_10_236_299, internal_id_manager);
        CHECK((1 == raster_with_one_pixel.width() && 1 == raster_with_one_pixel.width()));
        CHECK((1 == raster_with_one_pixel.width() && 1 == raster_with_one_pixel.height()));
        CHECK(internal_id_manager.convert_region_id_to_internal_id("NO-3035") == raster_with_one_pixel.pixel(glm::uvec2(0, 0)));
    }
}

TEST_CASE("nucleus/EAWS Reports")
{
    nucleus::avalanche::ReportLoadService reportLoadService;
    QSignalSpy spy(&reportLoadService, &nucleus::avalanche::ReportLoadService::load_from_TU_Wien_finished);
    reportLoadService.load_from_tu_wien(QDate::currentDate());

    spy.wait(10000);
    REQUIRE(spy.count() == 1);
    QList<QVariant> arguments = spy.takeFirst();
    tl::expected<std::vector<nucleus::avalanche::ReportTUWien>, QString> result
        = qvariant_cast<tl::expected<std::vector<nucleus::avalanche::ReportTUWien>, QString>>(arguments.at(0));
    if (result.has_value()) {
        std::vector<nucleus::avalanche::ReportTUWien> report = arguments.at(0).value<std::vector<nucleus::avalanche::ReportTUWien>>();
        CHECK(report.size() > 0);
    } else {
        std::cout << "\n##############\n##############\n##############\n##############" << result.error().toStdString();
    }
}
