/*
 * Copyright (C) 2020 NTT Corporation
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <memory>
#include <sys/types.h>
#include "anbox/container/lxc_container.h"

#include <gtest/gtest.h>

namespace anbox {
namespace container {
TEST(LxcContainer, IDMap) {
 auto config = get_id_map(static_cast<uid_t>(1000), static_cast<gid_t>(1000));
 EXPECT_EQ(6, config.size());
 EXPECT_STREQ("u 0 100000 999", config[0].c_str());
 EXPECT_STREQ("g 0 100000 999", config[1].c_str());
 EXPECT_STREQ("u 1000 1000 1", config[2].c_str());
 EXPECT_STREQ("g 1000 1000 1", config[3].c_str());
 // NOTE: host UID 100999 and 101000 are unmapped due to a historical reason
 // https://github.com/anbox/anbox/issues/1428#issuecomment-615377907
 EXPECT_STREQ("u 1001 101001 99000", config[4].c_str());
 EXPECT_STREQ("g 1001 101001 99000", config[5].c_str());

 config = get_id_map(static_cast<uid_t>(2000), static_cast<gid_t>(2000));
 EXPECT_EQ(6, config.size());
 EXPECT_STREQ("u 0 100000 999", config[0].c_str());
 EXPECT_STREQ("g 0 100000 999", config[1].c_str());
 EXPECT_STREQ("u 1000 2000 1", config[2].c_str());
 EXPECT_STREQ("g 1000 2000 1", config[3].c_str());
 EXPECT_STREQ("u 1001 101001 99000", config[4].c_str());
 EXPECT_STREQ("g 1001 101001 99000", config[5].c_str());
}

TEST(LxcContainer, GraphicsCompatibilityMountSelection) {
  const std::string gles =
      "/var/lib/anbox/state/vendor-gles-compat/lib64/libGLESv2_enc.so";
  const std::string mapper =
      "/var/lib/anbox/state/vendor-gfxstream-compat/lib64/hw/mapper.so";
  const std::string unrelated = "/var/lib/anbox/state/anbox.prop";

  EXPECT_TRUE(should_mount_graphics_compatibility_bind(false, gles));
  EXPECT_TRUE(should_mount_graphics_compatibility_bind(false, mapper));
  EXPECT_TRUE(should_mount_graphics_compatibility_bind(false, unrelated));

  EXPECT_FALSE(should_mount_graphics_compatibility_bind(true, gles));
  EXPECT_TRUE(should_mount_graphics_compatibility_bind(true, mapper));
  EXPECT_TRUE(should_mount_graphics_compatibility_bind(true, unrelated));
}

} // namespace container
} // namespace anbox
