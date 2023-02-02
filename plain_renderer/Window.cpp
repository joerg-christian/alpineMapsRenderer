/*****************************************************************************
 * Alpine Terrain Builder
 * Copyright (C) 2023 madam
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

#include "Window.h"

Window::Window()
{
    connect(&m_gl_window, &gl_engine::Window::update_requested, this, qOverload<>(&QOpenGLWindow::update));
}

void Window::initializeGL()
{
    m_gl_window.initialise_gpu();
}

void Window::resizeGL(int w, int h)
{
    m_gl_window.resize(w, h, devicePixelRatio());
}

void Window::paintGL()
{
    m_gl_window.paint();
}

void Window::paintOverGL()
{
    QPainter p(this);
    m_gl_window.paintOverGL(&p);
}

gl_engine::Window* Window::render_window()
{
    return &m_gl_window;
}

void Window::mousePressEvent(QMouseEvent* e)
{
    m_gl_window.mousePressEvent(e);
}

void Window::mouseMoveEvent(QMouseEvent* e)
{
    m_gl_window.mouseMoveEvent(e);
}

void Window::wheelEvent(QWheelEvent* e)
{
    m_gl_window.wheelEvent(e);
}

void Window::keyPressEvent(QKeyEvent* e)
{
    m_gl_window.keyPressEvent(e);
}

void Window::touchEvent(QTouchEvent* e)
{
    m_gl_window.touchEvent(e);
}