/*****************************************************************************
 * AlpineMaps.org
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

#include <QSignalSpy>
#include <QThread>
#include <catch2/catch_test_macros.hpp>

#include "nucleus/tile/SlotLimiter.h"
#include "nucleus/tile/types.h"
#include "radix/tile.h"

using namespace nucleus::tile;

TEST_CASE("nucleus/tile/slot limiter")
{
    SECTION("doesn't move when requesting empty array")
    {
        SlotLimiter sl;
        QSignalSpy spy(&sl, &SlotLimiter::quad_requested);
        sl.request_quads({});
        CHECK(spy.size() == 0);
    }

    SECTION("sends on requests")
    {
        SlotLimiter sl;
        QSignalSpy spy(&sl, &SlotLimiter::quad_requested);
        sl.request_quads({ Id { 0, { 0, 0 } }, Id { 1, { 0, 0 } } });
        REQUIRE(spy.size() == 2);
        CHECK(spy[0][0].value<Id>() == Id { 0, { 0, 0 } });
        CHECK(spy[1][0].value<Id>() == Id { 1, { 0, 0 } });
        CHECK(sl.slots_taken() == 2);
    }

    SECTION("sends on requests only up to the limit of tile slots")
    {
        SlotLimiter sl;
        sl.set_limit(2);
        QSignalSpy spy(&sl, &SlotLimiter::quad_requested);
        sl.request_quads({ Id { 0, { 0, 0 } }, Id { 1, { 0, 0 } }, Id { 1, { 0, 1 } } });
        CHECK(sl.slots_taken() == 2);
        REQUIRE(spy.size() == 2);
        CHECK(spy[0][0].value<Id>() == Id { 0, { 0, 0 } });
        CHECK(spy[1][0].value<Id>() == Id { 1, { 0, 0 } });

        sl.request_quads({ Id { 1, { 1, 0 } } });
        CHECK(sl.slots_taken() == 2);
        CHECK(spy.size() == 2);
    }

    SECTION("receiving tiles frees up slots")
    {
        SlotLimiter sl;
        sl.set_limit(2);
        QSignalSpy spy(&sl, &SlotLimiter::quad_requested);
        sl.request_quads({ Id { 0, { 0, 0 } }, Id { 1, { 0, 0 } } });
        CHECK(sl.slots_taken() == 2);
        REQUIRE(spy.size() == 2);

        sl.deliver_quad(DataQuad { Id { 0, { 0, 0 } } });
        CHECK(sl.slots_taken() == 1);
        sl.deliver_quad(DataQuad { Id { 1, { 0, 0 } } });
        CHECK(sl.slots_taken() == 0);
    }

    SECTION("receiving tiles triggers processing of queue")
    {
        SlotLimiter sl;
        sl.set_limit(2);
        QSignalSpy spy(&sl, &SlotLimiter::quad_requested);
        sl.request_quads({ Id { 0, { 0, 0 } }, Id { 1, { 0, 0 } }, Id { 1, { 0, 1 } }, Id { 2, { 0, 0 } } });
        CHECK(sl.slots_taken() == 2);
        REQUIRE(spy.size() == 2);

        sl.deliver_quad(DataQuad { Id { 0, { 0, 0 } } });
        CHECK(sl.slots_taken() == 2);
        REQUIRE(spy.size() == 3);
        CHECK(spy[2][0].value<Id>() == Id { 1, { 0, 1 } });

        sl.deliver_quad(DataQuad { Id { 1, { 0, 0 } } });
        CHECK(sl.slots_taken() == 2);
        REQUIRE(spy.size() == 4);
        CHECK(spy[3][0].value<Id>() == Id { 2, { 0, 0 } });

        // running out of tile requests
        sl.deliver_quad(DataQuad { Id { 1, { 0, 1 } } });
        CHECK(sl.slots_taken() == 1);
        sl.deliver_quad(DataQuad { Id { 2, { 0, 0 } } });
        CHECK(sl.slots_taken() == 0);
        CHECK(spy.size() == 4);

        sl.request_quads({ Id { 0, { 0, 0 } }, Id { 1, { 0, 0 } } });
        CHECK(sl.slots_taken() == 2);
        REQUIRE(spy.size() == 6);
        CHECK(spy[4][0].value<Id>() == Id { 0, { 0, 0 } });
        CHECK(spy[5][0].value<Id>() == Id { 1, { 0, 0 } });
    }

    SECTION("receiving tiles triggers processing of queue")
    {
        SlotLimiter sl;
        sl.set_limit(2);
        QSignalSpy spy(&sl, &SlotLimiter::quad_requested);
        sl.request_quads({ Id { 0, { 0, 0 } }, Id { 1, { 0, 0 } }, Id { 2, { 0, 0 } }, Id { 3, { 0, 0 } }, Id { 4, { 0, 0 } }, Id { 5, { 0, 0 } }, Id { 6, { 0, 0 } } });
        CHECK(sl.slots_taken() == 2);
        REQUIRE(spy.size() == 2);
        CHECK(spy[0][0].value<Id>() == Id { 0, { 0, 0 } });
        CHECK(spy[1][0].value<Id>() == Id { 1, { 0, 0 } });

        for (unsigned i = 0; i < 5; ++i) {
            sl.deliver_quad(DataQuad { Id { i, { 0, 0 } } });
            CHECK(sl.slots_taken() == 2);

            REQUIRE(spy.size() == int(3 + i));
            CHECK(spy[int(i + 2)][0].value<Id>() == Id { i + 2, { 0, 0 } });
        }
        CHECK(sl.slots_taken() == 2);
        REQUIRE(spy.size() == 7);
        CHECK(spy[6][0].value<Id>() == Id { 6, { 0, 0 } });

        // running out of tile requests
        sl.deliver_quad(DataQuad { Id { 5, { 0, 0 } } });
        CHECK(sl.slots_taken() == 1);
        sl.deliver_quad(DataQuad { Id { 6, { 0, 0 } } });
        CHECK(sl.slots_taken() == 0);
        CHECK(spy.size() == 7);

        sl.request_quads({ Id { 0, { 0, 0 } }, Id { 1, { 0, 0 } } });
        CHECK(sl.slots_taken() == 2);
        REQUIRE(spy.size() == 9);
        CHECK(spy[7][0].value<Id>() == Id { 0, { 0, 0 } });
        CHECK(spy[8][0].value<Id>() == Id { 1, { 0, 0 } });
    }

    SECTION("updating request list omits requesting in flight tiles again")
    {
        SlotLimiter sl;
        sl.set_limit(2);
        QSignalSpy spy(&sl, &SlotLimiter::quad_requested);
        sl.request_quads({ Id { 0, { 0, 0 } }, Id { 1, { 0, 0 } }, Id { 2, { 0, 0 } }, Id { 3, { 0, 0 } } });
        CHECK(sl.slots_taken() == 2);

        REQUIRE(spy.size() == 2);
        sl.request_quads({ Id { 0, { 0, 0 } }, // already requested
            Id { 1, { 0, 0 } }, // already requested
            Id { 2, { 1, 0 } }, // new, should go next
            Id { 3, { 1, 0 } } }); // new, should go next
        CHECK(sl.slots_taken() == 2);

        sl.deliver_quad(DataQuad { Id { 0, { 0, 0 } } });
        CHECK(sl.slots_taken() == 2);
        REQUIRE(spy.size() == 3);
        CHECK(spy[2][0].value<Id>() == Id { 2, { 1, 0 } });
        CHECK(sl.slots_taken() == 2);

        sl.deliver_quad(DataQuad { Id { 1, { 0, 0 } } });
        CHECK(sl.slots_taken() == 2);
        REQUIRE(spy.size() == 4);
        CHECK(spy[3][0].value<Id>() == Id { 3, { 1, 0 } });

        sl.deliver_quad(DataQuad { Id { 3, { 1, 0 } } });
        CHECK(sl.slots_taken() == 1);

        sl.deliver_quad(DataQuad { Id { 2, { 1, 0 } } });
        CHECK(sl.slots_taken() == 0);
    }

    SECTION("delivered quads are sent on")
    {
        SlotLimiter sl;
        QSignalSpy spy(&sl, &SlotLimiter::quad_delivered);
        sl.deliver_quad(DataQuad { Id { 0, { 0, 0 } }, 4, {} });
        REQUIRE(spy.size() == 1);
        CHECK(spy[0][0].value<DataQuad>().id == Id { 0, { 0, 0 } });

        sl.deliver_quad(DataQuad { Id { 1, { 2, 3 } }, 4, {} });
        REQUIRE(spy.size() == 2);
        CHECK(spy[1][0].value<DataQuad>().id == Id { 1, { 2, 3 } });
    }
}
