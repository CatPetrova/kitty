// License: GPLv3

#include "internal.h"

#include <X11/XKBlib.h>
#include <stdlib.h>
#include <string.h>

#define FCITX_SERVICE "org.freedesktop.portal.Fcitx"
#define FCITX_IM_INTERFACE "org.fcitx.Fcitx.InputMethod1"
#define FCITX_IC_INTERFACE "org.fcitx.Fcitx.InputContext1"
#define FCITX_SIGNALS "type='signal',sender='" FCITX_SERVICE "',interface='" FCITX_IC_INTERFACE "'"
#define FCITX_OWNER_CHANGES "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='" FCITX_SERVICE "'"

static DBusConnection *bus;

static void
send_ime_text(_GLFWwindow *window, GLFWIMEState state, const char *text, int cursor) {
    GLFWkeyevent ev = {
        .action = GLFW_PRESS,
        .ime_state = state,
        .text = text,
        .has_ime_cursor = cursor >= 0,
        .ime_cursor = cursor >= 0 ? (uint32_t)cursor : 0,
    };
    _glfwInputKeyboard(window, &ev);
}

static void
send_preedit(_GLFWwindow *window, DBusMessage *message) {
    if (!dbus_message_has_signature(message, "a(si)i")) return;
    DBusMessageIter args, segments, segment;
    dbus_message_iter_init(message, &args);
    dbus_message_iter_recurse(&args, &segments);
    size_t bytes = 0;
    for (; dbus_message_iter_get_arg_type(&segments) == DBUS_TYPE_STRUCT; dbus_message_iter_next(&segments)) {
        const char *part;
        dbus_message_iter_recurse(&segments, &segment);
        dbus_message_iter_get_basic(&segment, &part);
        bytes += strlen(part);
    }
    char *text = malloc(bytes + 1);
    if (!text) return;
    char *end = text;
    dbus_message_iter_recurse(&args, &segments);
    for (; dbus_message_iter_get_arg_type(&segments) == DBUS_TYPE_STRUCT; dbus_message_iter_next(&segments)) {
        const char *part;
        dbus_message_iter_recurse(&segments, &segment);
        dbus_message_iter_get_basic(&segment, &part);
        const size_t len = strlen(part);
        memcpy(end, part, len);
        end += len;
    }
    *end = 0;

    int32_t byte_offset;
    dbus_message_iter_next(&args);
    dbus_message_iter_get_basic(&args, &byte_offset);
    int cursor = -1;
    if (byte_offset >= 0 && (size_t)byte_offset <= bytes) {
        // Fcitx reports a UTF-8 byte offset. GLFW uses Unicode code points.
        cursor = 0;
        for (int32_t i = 0; i < byte_offset; i++) {
            if (((unsigned char)text[i] & 0xc0) != 0x80) cursor++;
        }
    }
    send_ime_text(window, GLFW_IME_PREEDIT_CHANGED, text, cursor);
    free(text);
}

static void
clear_context(_GLFWwindow *window) {
    free(window->x11.fcitx.path);
    free(window->x11.fcitx.owner);
    memset(&window->x11.fcitx, 0, sizeof(window->x11.fcitx));
}

static bool
create_context(_GLFWwindow *window) {
    if (window->x11.fcitx.path) return true;
    RAII_MSG(request, dbus_message_new_method_call(FCITX_SERVICE, "/org/freedesktop/portal/inputmethod", FCITX_IM_INTERFACE, "CreateInputContext"));
    if (!request) return false;
    dbus_message_set_auto_start(request, FALSE);
    DBusMessageIter args, properties, property;
    const char *properties_list[][2] = {{"program", "kitty"}, {"display", "x11:"}};
    dbus_message_iter_init_append(request, &args);
    if (!dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "(ss)", &properties)) return false;
    for (size_t i = 0; i < arraysz(properties_list); i++) {
        if (!dbus_message_iter_open_container(&properties, DBUS_TYPE_STRUCT, NULL, &property) ||
            !dbus_message_iter_append_basic(&property, DBUS_TYPE_STRING, &properties_list[i][0]) ||
            !dbus_message_iter_append_basic(&property, DBUS_TYPE_STRING, &properties_list[i][1]) || !dbus_message_iter_close_container(&properties, &property))
            return false;
    }
    if (!dbus_message_iter_close_container(&args, &properties)) return false;

    RAII_MSG(reply, dbus_connection_send_with_reply_and_block(bus, request, 500, NULL));
    if (!reply || !dbus_message_has_signature(reply, "oay")) return false;
    const char *owner = dbus_message_get_sender(reply), *path;
    if (!owner) return false;
    dbus_message_iter_init(reply, &args);
    dbus_message_iter_get_basic(&args, &path);
    window->x11.fcitx.path = _glfw_strdup(path);
    window->x11.fcitx.owner = _glfw_strdup(owner);

    // Enable formatted preedit and let Fcitx draw the candidate window.
    const uint64_t capabilities = (1u << 1) | (1u << 4);
    if (window->x11.fcitx.path && window->x11.fcitx.owner &&
        glfw_dbus_call_method_no_reply(bus, owner, path, FCITX_IC_INTERFACE, "SetCapability", DBUS_TYPE_UINT64, &capabilities, DBUS_TYPE_INVALID)) {
        return true;
    }
    glfw_dbus_call_method_no_reply(bus, owner, path, FCITX_IC_INTERFACE, "DestroyIC", DBUS_TYPE_INVALID);
    clear_context(window);
    return false;
}

static void
set_focus(_GLFWwindow *window, bool focused) {
    if (focused && (!_glfwPlatformWindowFocused(window) || !create_context(window))) return;
    if (!window->x11.fcitx.path) return;
    glfw_dbus_call_method_no_reply(
        bus, window->x11.fcitx.owner, window->x11.fcitx.path, FCITX_IC_INTERFACE, focused ? "FocusIn" : "FocusOut", DBUS_TYPE_INVALID);
    window->x11.fcitx.cursor_valid = false;
}

static unsigned int
forwarded_key_modifiers(uint32_t state) {
    // Fcitx can add virtual modifier bits when forwarding a key.
    unsigned int mods = glfw_x11_modifiers(state);
    if (state & (1u << 26)) mods |= GLFW_MOD_SUPER;
    if (state & (Mod3Mask | (1u << 27))) mods |= GLFW_MOD_HYPER;
    if (state & (1u << 28)) mods |= GLFW_MOD_META;
    return _glfwApplyModifierRemap(mods);
}

static DBusHandlerResult
handle_signal(DBusConnection *connection UNUSED, DBusMessage *message, void *data UNUSED) {
    if (dbus_message_is_signal(message, DBUS_INTERFACE_DBUS, "NameOwnerChanged") && dbus_message_has_sender(message, DBUS_SERVICE_DBUS)) {
        const char *name, *previous_owner, *new_owner;
        if (dbus_message_get_args(message, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &previous_owner, DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID) &&
            strcmp(name, FCITX_SERVICE) == 0) {
            for (_GLFWwindow *window = _glfw.windowListHead; window;) {
                if (window->x11.fcitx.owner && strcmp(window->x11.fcitx.owner, previous_owner) == 0) {
                    clear_context(window);
                    send_ime_text(window, GLFW_IME_PREEDIT_CHANGED, "", -1);
                    // The keyboard callback can close or create windows.
                    window = _glfw.windowListHead;
                } else window = window->next;
            }
            _GLFWwindow *focused = _glfwFocusedWindow();
            if (*new_owner && focused) set_focus(focused, true);
        }
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    const int signal = glfw_dbus_match_signal(message, FCITX_IC_INTERFACE, "CommitString", "UpdateFormattedPreedit", "ForwardKey", NULL);
    if (signal < 0) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    for (_GLFWwindow *window = _glfw.windowListHead; window; window = window->next) {
        // Fcitx 4 broadcasts these signals, including commits sent on focus loss.
        if (!window->x11.fcitx.path || !dbus_message_has_path(message, window->x11.fcitx.path) || !dbus_message_has_sender(message, window->x11.fcitx.owner))
            continue;
        switch (signal) {
            case 0: {
                const char *text;
                if (dbus_message_get_args(message, NULL, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID)) { send_ime_text(window, GLFW_IME_COMMIT_TEXT, text, -1); }
                break;
            }
            case 1: send_preedit(window, message); break;
            case 2: {
                uint32_t sym, state;
                dbus_bool_t released;
                if (dbus_message_get_args(message, NULL, DBUS_TYPE_UINT32, &sym, DBUS_TYPE_UINT32, &state, DBUS_TYPE_BOOLEAN, &released, DBUS_TYPE_INVALID)) {
                    glfw_xkb_forward_fcitx_key(window, sym, forwarded_key_modifiers(state), released ? GLFW_RELEASE : GLFW_PRESS);
                }
                break;
            }
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void
glfw_fcitx_init(void) {
    const char *module = getenv("GLFW_IM_MODULE");
    const char *modifiers = getenv("XMODIFIERS");
    const bool enabled = (module && *module) ? strcmp(module, "fcitx") == 0 : (modifiers && strcmp(modifiers, "@im=fcitx") == 0);
    if (!enabled) return;
    bus = glfw_dbus_session_bus();
    if (!bus) return;
    if (!dbus_connection_add_filter(bus, handle_signal, NULL, NULL)) {
        bus = NULL;
        return;
    }
    dbus_bus_add_match(bus, FCITX_SIGNALS, NULL);
    dbus_bus_add_match(bus, FCITX_OWNER_CHANGES, NULL);
}

void
glfw_fcitx_terminate(void) {
    if (!bus) return;
    dbus_bus_remove_match(bus, FCITX_OWNER_CHANGES, NULL);
    dbus_bus_remove_match(bus, FCITX_SIGNALS, NULL);
    dbus_connection_remove_filter(bus, handle_signal, NULL);
    bus = NULL;
}

void
glfw_fcitx_destroy_context(_GLFWwindow *window) {
    if (window->x11.fcitx.path) {
        glfw_dbus_call_method_no_reply(bus, window->x11.fcitx.owner, window->x11.fcitx.path, FCITX_IC_INTERFACE, "DestroyIC", DBUS_TYPE_INVALID);
        clear_context(window);
    }
}

bool
glfw_fcitx_update_ime_state(_GLFWwindow *window, const GLFWIMEUpdateEvent *ev) {
    if (!bus) return false;
    if (ev->type == GLFW_IME_UPDATE_FOCUS) {
        if (ev->cancel_preedit && window->x11.fcitx.path) {
            // A pane switch must cancel composition. Fcitx can commit on FocusOut,
            // and signals from the old context must not reach the new pane.
            glfw_dbus_call_method_no_reply(bus, window->x11.fcitx.owner, window->x11.fcitx.path, FCITX_IC_INTERFACE, "Reset", DBUS_TYPE_INVALID);
            glfw_fcitx_destroy_context(window);
        } else set_focus(window, ev->focused);
    } else if (ev->type == GLFW_IME_UPDATE_CURSOR_POSITION && window->x11.fcitx.path) {
        int x, y;
        _glfwPlatformGetWindowPos(window, &x, &y);
        const int32_t cursor[] = {x + ev->cursor.left, y + ev->cursor.top, ev->cursor.width, ev->cursor.height};
        if (!window->x11.fcitx.cursor_valid || memcmp(window->x11.fcitx.cursor, cursor, sizeof(cursor)) != 0) {
            if (glfw_dbus_call_method_no_reply(
                    bus,
                    window->x11.fcitx.owner,
                    window->x11.fcitx.path,
                    FCITX_IC_INTERFACE,
                    "SetCursorRect",
                    DBUS_TYPE_INT32,
                    &cursor[0],
                    DBUS_TYPE_INT32,
                    &cursor[1],
                    DBUS_TYPE_INT32,
                    &cursor[2],
                    DBUS_TYPE_INT32,
                    &cursor[3],
                    DBUS_TYPE_INVALID)) {
                memcpy(window->x11.fcitx.cursor, cursor, sizeof(cursor));
                window->x11.fcitx.cursor_valid = true;
            }
        }
    }
    return true;
}

typedef struct {
    GLFWid window_id;
    GLFWkeyevent event;
    char text[64];
} PendingKey;

static void
key_processed(DBusPendingCall *pending, void *data) {
    RAII_MSG(reply, dbus_pending_call_steal_reply(pending));
    if (!reply) return;
    PendingKey *key = data;
    if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
        // A timeout does not tell us whether Fcitx consumed this key.
        if (!dbus_message_is_error(reply, DBUS_ERROR_NAME_HAS_NO_OWNER) && !dbus_message_is_error(reply, DBUS_ERROR_SERVICE_UNKNOWN) &&
            !dbus_message_is_error(reply, DBUS_ERROR_UNKNOWN_OBJECT))
            return;
    } else {
        dbus_bool_t handled;
        if (!dbus_message_get_args(reply, NULL, DBUS_TYPE_BOOLEAN, &handled, DBUS_TYPE_INVALID) || handled) return;
    }
    _GLFWwindow *window = _glfwWindowForId(key->window_id);
    if (window) _glfwInputKeyboard(window, &key->event);
}

bool
glfw_fcitx_process_key(_GLFWwindow *window, const GLFWkeyevent *ev, uint32_t state, uint32_t timestamp) {
    if (!bus || !window->x11.fcitx.path) return false;
    RAII_MSG(request, dbus_message_new_method_call(window->x11.fcitx.owner, window->x11.fcitx.path, FCITX_IC_INTERFACE, "ProcessKeyEvent"));
    const uint32_t keycode = ev->native_key_id;
    unsigned int consumed_modifiers;
    KeySym keysym;
    // Send native modifiers and a keysym from the same state to Fcitx. The
    // saved GLFW event below retains kitty's remap_modifier for fallback.
    if (!XkbLookupKeySym(_glfw.x11.display, (KeyCode)keycode, state, &consumed_modifiers, &keysym)) return false;
    const uint32_t sym = (uint32_t)keysym;
    const dbus_bool_t released = ev->action == GLFW_RELEASE;
    if (!request || !dbus_message_append_args(
                        request,
                        DBUS_TYPE_UINT32,
                        &sym,
                        DBUS_TYPE_UINT32,
                        &keycode,
                        DBUS_TYPE_UINT32,
                        &state,
                        DBUS_TYPE_BOOLEAN,
                        &released,
                        DBUS_TYPE_UINT32,
                        &timestamp,
                        DBUS_TYPE_INVALID))
        return false;
    PendingKey *key = calloc(1, sizeof(*key));
    if (!key) return false;
    key->window_id = window->id;
    key->event = *ev;
    if (ev->text) {
        snprintf(key->text, sizeof(key->text), "%s", ev->text);
        key->event.text = key->text;
    }
    DBusPendingCall *pending = NULL;
    if (!dbus_connection_send_with_reply(bus, request, &pending, 3000) || !pending) {
        free(key);
        return false;
    }
    const bool queued = dbus_pending_call_set_notify(pending, key_processed, key, free);
    if (!queued) {
        dbus_pending_call_cancel(pending);
        free(key);
    }
    dbus_pending_call_unref(pending);
    return queued;
}
