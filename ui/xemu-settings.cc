/*
 * xemu Settings Management
 *
 * Copyright (C) 2020-2022 Matt Borgerson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include <stdlib.h>
#include <SDL3/SDL_filesystem.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <toml++/toml.h>
#include <cnode.h>
#include <sstream>
#include <iostream>
#include <locale.h>

#include "xemu-controllers.h"
#include "xemu-settings.h"

#define DEFINE_CONFIG_TREE
#include "xemu-config.h"

struct config g_config;

static const char *filename = "xemu.toml";
static const char *chihiro_filename = "chihiro.toml";
static const char *settings_path;
static const char *chihiro_settings_path;
static std::string error_msg;

const char *xemu_settings_get_error_message(void)
{
    return error_msg.length() ? error_msg.c_str() : NULL;
}

static bool xemu_settings_detect_portable_mode(void)
{
    const char *base = SDL_GetBasePath();
    if (!base) {
        return false;
    }

    char *portable_chihiro = g_strdup_printf("%s%s", base, chihiro_filename);
    FILE *tmpfile;
    if ((tmpfile = qemu_fopen(portable_chihiro, "r"))) {
        fclose(tmpfile);
        g_free(portable_chihiro);
        return true;
    }
    g_free(portable_chihiro);

    char *portable_path = g_strdup_printf("%s%s", base, filename);
    if ((tmpfile = qemu_fopen(portable_path, "r"))) {
        fclose(tmpfile);
        g_free(portable_path);
        return true;
    }
    g_free(portable_path);

    return false;
}

void xemu_settings_set_path(const char *path)
{
    assert(path != NULL);
    assert(settings_path == NULL);
    settings_path = path;
    fprintf(stderr, "%s: config path: %s\n", __func__, settings_path);
}

void xemu_settings_set_chihiro_path(const char *path)
{
    assert(path != NULL);
    assert(chihiro_settings_path == NULL);
    chihiro_settings_path = path;
    fprintf(stderr, "%s: chihiro config path: %s\n", __func__, chihiro_settings_path);
}

const char *xemu_settings_get_base_path(void)
{
    static const char *base_path = NULL;
    if (base_path != NULL) {
        return base_path;
    }

    if (xemu_settings_detect_portable_mode()) {
        const char *base = SDL_GetBasePath();
        assert(base != NULL);
        base_path = g_strdup(base);
    } else {
        char *base = SDL_GetPrefPath("xemu", "xemu");
        assert(base != NULL);
        base_path = g_strdup(base);
        SDL_free(base);
    }
    fprintf(stderr, "%s: base path: %s\n", __func__, base_path);
    return base_path;
}

const char *xemu_settings_get_path(void)
{
    if (settings_path != NULL) {
        return settings_path;
    }

    const char *base = xemu_settings_get_base_path();
    assert(base != NULL);
    settings_path = g_strdup_printf("%s%s", base, filename);
    fprintf(stderr, "%s: config path: %s\n", __func__, settings_path);
    return settings_path;
}

const char *xemu_settings_get_chihiro_path(void)
{
    if (chihiro_settings_path != NULL) {
        return chihiro_settings_path;
    }

    const char *base = xemu_settings_get_base_path();
    assert(base != NULL);
    chihiro_settings_path = g_strdup_printf("%s%s", base, chihiro_filename);
    fprintf(stderr, "%s: chihiro config path: %s\n", __func__, chihiro_settings_path);
    return chihiro_settings_path;
}

const char *xemu_settings_get_default_eeprom_path(void)
{
    static char *eeprom_path = NULL;
    if (eeprom_path != NULL) {
        return eeprom_path;
    }

    const char *base = xemu_settings_get_base_path();
    assert(base != NULL);
    eeprom_path = g_strdup_printf("%s%s", base, "eeprom.bin");
    return eeprom_path;
}

static ssize_t get_file_size(FILE *fd)
{
    if (fseek(fd, 0, SEEK_END)) {
        return -1;
    }

    size_t file_size = ftell(fd);

    if (fseek(fd, 0, SEEK_SET)) {
        return -1;
    }

    return file_size;
}

static const char *read_file(FILE *fd)
{
    ssize_t size = get_file_size(fd);
    if (size < 0) {
        return NULL;
    }

    char *buf = (char *)malloc(size + 1);
    if (!buf) {
        return NULL;
    }

    if (size > 0) {
        if (fread(buf, size, 1, fd) != 1) {
            free(buf);
            return NULL;
        }
    }

    buf[size] = '\x00';
    return buf;
}

static bool parse_and_apply_toml_file(const char *path, std::string &err_out)
{
    if (qemu_access(path, F_OK) == -1) {
        return false;
    }

    FILE *fd = qemu_fopen(path, "rb");
    if (!fd) {
        err_out = std::string("Failed to open config file for reading: ") + path + "\n";
        return false;
    }

    const char *buf = read_file(fd);
    fclose(fd);
    if (!buf) {
        err_out = std::string("Failed to read config file: ") + path + "\n";
        return false;
    }

    char *previous_numeric_locale = setlocale(LC_NUMERIC, NULL);
    if (previous_numeric_locale) {
        previous_numeric_locale = g_strdup(previous_numeric_locale);
    }
    setlocale(LC_NUMERIC, "C");

    bool ok = false;
    try {
        config_tree.update_from_table(toml::parse(buf));
        ok = true;
    } catch (const toml::parse_error &err) {
        std::ostringstream oss;
        oss << "Error parsing config file at " << err.source().begin << " (" << path << "):\n"
            << "    " << err.description() << "\n"
            << "Please fix the error or delete the file to continue.\n";
        err_out = oss.str();
    }
    free((char *)buf);

    if (previous_numeric_locale) {
        setlocale(LC_NUMERIC, previous_numeric_locale);
        g_free(previous_numeric_locale);
    }

    return ok;
}

bool xemu_settings_load(void)
{
    const char *settings_path = xemu_settings_get_path();
    const char *chihiro_path = xemu_settings_get_chihiro_path();
    bool loaded_any = false;

    /* 1. Load common settings from xemu.toml (if present) */
    std::string err;
    if (parse_and_apply_toml_file(settings_path, err)) {
        loaded_any = true;
        fprintf(stderr, "%s: loaded common settings from %s\n", __func__, settings_path);
    } else if (!err.empty()) {
        error_msg += err;
    }

    /* 2. Load Chihiro-specific settings and overrides from chihiro.toml (higher priority) */
    err.clear();
    if (parse_and_apply_toml_file(chihiro_path, err)) {
        loaded_any = true;
        fprintf(stderr, "%s: loaded chihiro settings from %s\n", __func__, chihiro_path);
    } else if (!err.empty()) {
        error_msg += err;
    }

    if (!loaded_any && error_msg.empty()) {
        fprintf(stderr, "No config files found, starting with default settings.\n");
    }

    /* CHIHIRO (not upstream): OutRun 2's own gear keys are now the gear
     * paddles every driving game shares; keys set there move over once. */
    CNode *jvs = config_tree.child("chihiro")->child("jvs");
    for (const char *key : { "gear_up", "gear_down" }) {
        CNode *old = jvs->child("or2")->child(key);
        if (old->differs_from_default()) {
            jvs->child(key)->data.integer.val = old->data.integer.val;
            old->reset_to_defaults();
        }
    }
    /* The card games' own Card In keys are now one per player. */
    for (const char *game : { "gundam", "wmmt2", "gs" }) {
        CNode *old = jvs->child(game)->child("card");
        if (old->differs_from_default()) {
            jvs->child("card_in")->data.integer.val = old->data.integer.val;
            old->reset_to_defaults();
        }
    }
    CNode *jvs_p2 = config_tree.child("chihiro")->child("jvs_p2");
    CNode *old_p2 = jvs_p2->child("gs_card");
    if (old_p2->differs_from_default()) {
        jvs_p2->child("card_in")->data.integer.val = old_p2->data.integer.val;
        old_p2->reset_to_defaults();
    }
    /* Snapshots are no longer bound to F5-F8: quick saves are per game. */
    config_tree.child("general")->child("snapshots")->child("shortcuts")
        ->reset_to_defaults();

    config_tree.store_to_struct(&g_config);

    return error_msg.empty();
}

void xemu_settings_save(void)
{
    char *previous_numeric_locale = setlocale(LC_NUMERIC, NULL);
    if (previous_numeric_locale) {
        previous_numeric_locale = g_strdup(previous_numeric_locale);
    }

    /* Ensure numeric values are printed with '.' radix, no grouping */
    setlocale(LC_NUMERIC, "C");

    // The global controller vibration setting is replaced with a per-controller config.
    // xemu_settings_load_gamepad_mapping should have migrated that setting to any connected
    // controller, so we can set it to true (default) now to remove it from the user config.
    g_config.input.allow_vibration = true;

    config_tree.update_from_struct(&g_config);

    /* 1. Save chihiro.toml: holds the [chihiro] section exclusively */
    CNode *chihiro_node = config_tree.child("chihiro");
    if (chihiro_node) {
        const char *chihiro_path = xemu_settings_get_chihiro_path();
        std::string chihiro_toml = chihiro_node->generate_delta_toml("", false, 0, false);
        if (!chihiro_toml.empty() || qemu_access(chihiro_path, F_OK) == 0) {
            FILE *cfd = qemu_fopen(chihiro_path, "wb");
            if (cfd) {
                fprintf(cfd, "%s", chihiro_toml.c_str());
                fclose(cfd);
            } else {
                fprintf(stderr, "Failed to open chihiro config file for writing: %s\n", chihiro_path);
            }
        }
    }

    /* 2. Save xemu.toml: holds common sections (everything EXCEPT [chihiro]) */
    const char *settings_path = xemu_settings_get_path();
    FILE *fd = qemu_fopen(settings_path, "wb");
    if (fd) {
        std::string xemu_toml = "";
        for (auto &c : config_tree.children) {
            if (c.name == "chihiro") {
                continue;
            }
            xemu_toml += c.generate_delta_toml("", false, 0, false);
        }
        fprintf(fd, "%s", xemu_toml.c_str());
        fclose(fd);
    } else {
        fprintf(stderr, "Failed to open config file for writing. Check permissions.\n");
    }

    if (previous_numeric_locale) {
        setlocale(LC_NUMERIC, previous_numeric_locale);
        g_free(previous_numeric_locale);
    }
}

void add_net_nat_forward_ports(int host, int guest, CONFIG_NET_NAT_FORWARD_PORTS_PROTOCOL protocol)
{
    // FIXME: - Realloc the arrays instead of free/alloc
    //        - Don't need to copy as much
    auto cnode = config_tree.child("net")
                            ->child("nat")
                            ->child("forward_ports");
    cnode->update_from_struct(&g_config);
    cnode->children.push_back(*cnode->array_item_type);
    auto &e = cnode->children.back();
    e.child("host")->set_integer(host);
    e.child("guest")->set_integer(guest);
    e.child("protocol")->set_enum_by_index(protocol);
    cnode->free_allocations(&g_config);
    cnode->store_to_struct(&g_config);
}

void remove_net_nat_forward_ports(unsigned int index)
{
    auto cnode = config_tree.child("net")
                            ->child("nat")
                            ->child("forward_ports");
    cnode->update_from_struct(&g_config);
    cnode->children.erase(cnode->children.begin()+index);
    cnode->free_allocations(&g_config);
    cnode->store_to_struct(&g_config);
}

bool xemu_settings_load_gamepad_mapping(const char *guid,
                                        GamepadMappings **mapping)
{
    unsigned int i;
    unsigned int gamepad_mappings_count = g_config.input.gamepad_mappings_count;
    for (i = 0; i < gamepad_mappings_count; ++i) {
        *mapping = &g_config.input.gamepad_mappings[i];
        if (strcmp((*mapping)->gamepad_id, guid) != 0) {
            continue;
        }

        // Migrate global 'allow_vibration' setting to the controller config
        if (!g_config.input.allow_vibration) {
            (*mapping)->enable_rumble = g_config.input.allow_vibration;
        }

        return false;
    }

    auto cnode = config_tree.child("input")->child("gamepad_mappings");
    cnode->update_from_struct(&g_config);
    cnode->free_allocations(&g_config);

    cnode->children.push_back(*cnode->array_item_type);
    CNode *mapping_node = &cnode->children.back();

    mapping_node->child("gamepad_id")->set_string(guid);

    cnode->store_to_struct(&g_config);

    *mapping =
        &g_config.input
             .gamepad_mappings[g_config.input.gamepad_mappings_count - 1];

    // Migrate global 'allow_vibration' setting to the controller config
    if (!g_config.input.allow_vibration) {
        (*mapping)->enable_rumble = g_config.input.allow_vibration;
    }

    return true;
}

void xemu_settings_reset_controller_mapping(const char *guid)
{
    unsigned int gamepad_mappings_count = g_config.input.gamepad_mappings_count;

    unsigned int i;
    struct config::input::gamepad_mappings *mapping;

    for (i = 0; i < gamepad_mappings_count; ++i) {
        mapping = &g_config.input.gamepad_mappings[i];
        if (strcmp(mapping->gamepad_id, guid) == 0) {
            break;
        }
    }

    if (i == gamepad_mappings_count) {
        return;
    }

    CNode *cnode = config_tree.child("input")->child("gamepad_mappings");
    cnode->update_from_struct(&g_config);

    // Careful not to free the mapping array, as other controllers may be using
    // it
    CNode *mapping_node = &cnode->children[i];
    mapping_node->reset_to_defaults();
    mapping_node->child("gamepad_id")->set_string(guid);
    mapping_node->store_to_struct(mapping);
}

void xemu_settings_reset_keyboard_mapping(void)
{
  auto cnode = config_tree.child("input")->child("keyboard_controller_scancode_map");
  cnode->update_from_struct(&g_config);
  cnode->reset_to_defaults();
  cnode->store_to_struct(&g_config);
}

/* CHIHIRO (not upstream) */
bool xemu_media_is_xbox_disc(const char *path)
{
    /* The XISO volume descriptor sits in sector 32, at the start of the
     * game partition: offset 0 of an XISO, after the video partition of a
     * redump image. */
    static const char magic[] = "MICROSOFT*XBOX*MEDIA";
    static const long partitions[] = { 0, 0x18300000 };
    if (!path || !path[0] || g_file_test(path, G_FILE_TEST_IS_DIR)) {
        return false;
    }
    FILE *f = qemu_fopen(path, "rb");
    if (!f) {
        return false;
    }
    bool disc = false;
    for (int i = 0; i < 2 && !disc; i++) {
        char buf[sizeof(magic) - 1];
        disc = fseek(f, partitions[i] + 0x10000, SEEK_SET) == 0 &&
               fread(buf, 1, sizeof(buf), f) == sizeof(buf) &&
               memcmp(buf, magic, sizeof(buf)) == 0;
    }
    fclose(f);
    return disc;
}

/* CHIHIRO (not upstream): -1 when the command line does not ask. */
static int chihiro_mode_asked = -1;

void xemu_chihiro_mode_ask(bool chihiro)
{
    chihiro_mode_asked = chihiro;
}

const char *xemu_chihiro_image(void)
{
    static char *image;
    if (!image) {
        const char *path = g_config.sys.files.dvd_path;
        image = g_strdup(path ? path : "");
    }
    return image;
}

bool xemu_chihiro_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *image = xemu_chihiro_image();
        if (chihiro_mode_asked >= 0) {
            mode = chihiro_mode_asked;
        } else if (image && image[0]) {
            mode = !xemu_media_is_xbox_disc(image);
        } else {
            switch (g_config.sys.default_machine) {
            case CONFIG_SYS_DEFAULT_MACHINE_XBOX:
                mode = 0;
                break;
            case CONFIG_SYS_DEFAULT_MACHINE_CHIHIRO:
                mode = 1;
                break;
            default:
                mode = g_config.sys.last_machine ==
                       CONFIG_SYS_LAST_MACHINE_CHIHIRO;
                break;
            }
        }
    }
    return mode;
}
