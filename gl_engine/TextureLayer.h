/*****************************************************************************
 * AlpineMaps.org
 * Copyright (C) 2024 Adam Celarek
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

#include "Texture.h"
#include <QObject>
#include <nucleus/Raster.h>
#include <nucleus/tile/DrawListGenerator.h>
#include <nucleus/tile/GpuArrayHelper.h>
#include <nucleus/tile/types.h>

namespace camera {
class Definition;
}

class QOpenGLShaderProgram;
class QOpenGLBuffer;
class QOpenGLVertexArrayObject;

namespace gl_engine {
class ShaderRegistry;
class ShaderProgram;
class TileGeometry;

class TextureLayer : public QObject {
    Q_OBJECT
public:
    explicit TextureLayer(QObject* parent = nullptr);
    void init(ShaderRegistry* shader_registry,
        const Texture::Format& ortho_texture_format = Texture::Format::CompressedRGBA8,
        const QString& vertex_shader = "tile.vert",
        const QString& fragment_shader = "tile.frag"); // needs OpenGL context
    void draw(const TileGeometry& tile_geometry,
        const nucleus::camera::Definition& camera,
        const nucleus::tile::DrawListGenerator::TileSet& draw_tiles,
        bool sort_tiles,
        glm::dvec3 sort_position) const;

private:
    // wrapper for template since thi is a slot which cannot be a template
    template <typename T> void update_gpu_quads_template(const std::vector<T>& new_quads, const std::vector<nucleus::tile::Id>& deleted_quads);

public slots:
    // Template works for nucleus::tile::GpuEawsQuad or nucleus::tile::GpuTextureQuad
    void update_gpu_quads(const std::vector<nucleus::tile::GpuTextureQuad>& new_quads, const std::vector<nucleus::tile::Id>& deleted_quads);
    void update_gpu_eaws_quads(const std::vector<nucleus::tile::GpuEawsQuad>& new_quads, const std::vector<nucleus::tile::Id>& deleted_quads);
    void set_quad_limit(unsigned new_limit);

private:
    void update_gpu_id_map();

    static constexpr auto ORTHO_RESOLUTION = 256;

    std::shared_ptr<ShaderProgram> m_shader;
    std::unique_ptr<Texture> m_ortho_textures;
    std::unique_ptr<Texture> m_tile_id_texture;
    std::unique_ptr<Texture> m_array_index_texture;

    nucleus::tile::GpuArrayHelper m_gpu_array_helper;
};
} // namespace gl_engine
