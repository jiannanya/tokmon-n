#include "title_edit_harness.h"

#include <slint-platform.h>
#include <private/slint_tests_helpers.h>

#include <cstdlib>
#include <cmath>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using slint::testing::ElementHandle;
using slint::private_api::testing::get_mocked_time;
using slint::private_api::testing::mock_elapsed_time;
using slint::private_api::testing::send_key_combo;
using slint::private_api::testing::send_keyboard_key_text;
using slint::private_api::testing::send_keyboard_string_sequence;
using slint::private_api::testing::send_mouse_click;

[[noreturn]] void fail(std::string_view message)
{
    std::cerr << "title edit UI test failed: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void expect(bool condition, std::string_view message)
{
    if (!condition) {
        fail(message);
    }
}

std::string as_string(const slint::SharedString &value)
{
    return std::string(value.data(), value.size());
}

template<typename Component>
ElementHandle find_element(const slint::ComponentHandle<Component> &component,
                           std::string_view local_id)
{
    auto found = ElementHandle::visit_elements(
            component, [local_id](const ElementHandle &element) -> std::optional<ElementHandle> {
                const auto id = element.id();
                if (!id) {
                    return std::nullopt;
                }

                const auto qualified_id = as_string(*id);
                const auto separator = qualified_id.rfind("::");
                const auto element_local_id = separator == std::string::npos
                        ? std::string_view(qualified_id)
                        : std::string_view(qualified_id).substr(separator + 2);
                return element_local_id == local_id ? std::make_optional(element) : std::nullopt;
            });
    if (!found) {
        fail(std::string("element not found: ") + std::string(local_id));
    }
    return *found;
}

template<typename Component>
void click_element(const slint::ComponentHandle<Component> &component, std::string_view local_id,
                   float relative_x = 0.5F)
{
    const auto element = find_element(component, local_id);
    const auto position = element.absolute_position();
    const auto size = element.size();
    send_mouse_click(component.operator->(), position.x + size.width * relative_x,
                     position.y + size.height * 0.5F);
}

void advance_time(std::uint64_t milliseconds)
{
    mock_elapsed_time(static_cast<std::int64_t>(get_mocked_time() + milliseconds));
    slint::platform::update_timers_and_animations();
}

} // namespace

int main()
{
    slint::testing::init();
    auto ui = TitleEditHarness::create();
    ui->show();
    slint::platform::update_timers_and_animations();
    advance_time(10);

    ui->invoke_test_select_model("deepseek", "deepseek-v4-flash");
    expect(as_string(ui->get_test_setting_name()) == "deepseek",
           "model selection must switch the configuration name and model as one pair");
    expect(as_string(ui->get_test_setting_main_model()) == "deepseek-v4-flash" &&
                   as_string(ui->get_test_model_name()) == "deepseek-v4-flash",
           "model selection must keep settings and composer labels synchronized");

    // Popup sanity check: show + click inside a PopupWindow must work.
    {
        click_element(ui, "mini-button");
        slint::platform::update_timers_and_animations();
        send_mouse_click(ui.operator->(), 850.0F, 150.0F);
        slint::platform::update_timers_and_animations();
        expect(ui->get_mini_popup_clicks() == 1,
               "a click inside a shown PopupWindow must reach the popup content");

        // Same pattern as SelectBox: popup hanging below a nested component.
        const auto mini_box = find_element(ui, "mini-select");
        const auto mini_pos = mini_box.absolute_position();
        const auto mini_size = mini_box.size();
        click_element(ui, "mini-select");
        slint::platform::update_timers_and_animations();
        send_mouse_click(ui.operator->(), mini_pos.x + mini_size.width * 0.5F,
                         mini_pos.y + mini_size.height + 6.0F + 49.0F);
        slint::platform::update_timers_and_animations();
        std::cerr << "mini-select picked: " << as_string(ui->get_mini_select_picked()) << '\n';
    }

    ui->set_test_error_title("配置文件无效");
    ui->set_test_error_message("unknown YAML field 'efault'");
    ui->set_test_error_fatal(true);
    ui->set_test_error_open(true);
    slint::platform::update_timers_and_animations();
    click_element(ui, "edit-title-button");
    expect(!ui->get_test_title_editing(),
           "the Slint configuration dialog must block the underlying window");
    click_element(ui, "accept-touch");
    expect(!ui->get_test_error_open(),
           "the themed configuration dialog button must close the overlay");
    expect(ui->get_error_dismiss_count() == 1,
           "closing the themed dialog must notify the application exactly once");

    send_key_combo(ui.operator->(), { "\x11", "n" });
    expect(ui->get_new_session_count() == 1,
           "the zero-size shortcut scope must retain Ctrl+N handling");

    ui->set_test_session_title("alpha beta");
    click_element(ui, "edit-title-button");
    expect(ui->get_test_title_editing(), "the pencil button must enter edit mode");
    advance_time(5);

    // A single click inside the text field must move the caret, not save and
    // close the editor. Waiting past the blur debounce catches regressions in
    // focus event ordering as well as immediate-save regressions.
    click_element(ui, "title-edit-input", 0.25F);
    advance_time(60);
    expect(ui->get_test_title_editing(), "one input click must keep edit mode active");
    expect(ui->get_rename_count() == 0, "one input click must not rename the session");

    send_keyboard_string_sequence(ui.operator->(), "X");
    const auto confirm_button = find_element(ui, "confirm-title-button");
    const auto confirm_position = confirm_button.absolute_position();
    const auto confirm_size = confirm_button.size();
    ui->window().dispatch_pointer_move_event(slint::LogicalPosition(
            { confirm_position.x + confirm_size.width * 0.5F,
              confirm_position.y + confirm_size.height * 0.5F }));
    slint::platform::update_timers_and_animations();
    expect(ui->get_test_title_confirm_hovered(),
           "the confirmation button must receive pointer hover");
    click_element(ui, "confirm-title-button");
    expect(!ui->get_test_title_editing(), "the confirmation button must leave edit mode");
    expect(ui->get_rename_count() == 1, "confirmation must save exactly once");
    const auto confirmed_title = as_string(ui->get_test_session_title());
    expect(confirmed_title.find('X') != std::string::npos && confirmed_title != "X",
           "one input click must collapse selection and insert at the clicked caret");
    expect(as_string(ui->get_last_renamed_title()) == as_string(ui->get_test_session_title()),
           "the displayed and persisted titles must match after confirmation");

    // Re-entering selects the existing title. Typing replaces it, and moving
    // focus outside must perform the edit -> non-edit transition and retain it.
    click_element(ui, "edit-title-button");
    advance_time(5);
    send_keyboard_string_sequence(ui.operator->(), "Outside save");
    click_element(ui, "composer-edit");
    slint::platform::update_timers_and_animations();
    advance_time(60);
    expect(!ui->get_test_title_editing(), "focus leaving the editor must leave edit mode");
    expect(ui->get_rename_count() == 2, "focus leaving the editor must save exactly once");
    expect(as_string(ui->get_test_session_title()) == "Outside save",
           "the title must be retained after leaving edit mode");
    expect(as_string(ui->get_last_renamed_title()) == "Outside save",
           "the retained title must be sent to persistence");

    click_element(ui, "edit-title-button");
    advance_time(5);
    send_keyboard_string_sequence(ui.operator->(), "Enter save");
    send_keyboard_key_text(ui.operator->(), "\n", true);
    send_keyboard_key_text(ui.operator->(), "\n", false);
    expect(!ui->get_test_title_editing(), "Enter must leave edit mode");
    expect(ui->get_rename_count() == 3, "Enter must save exactly once");
    expect(as_string(ui->get_test_session_title()) == "Enter save",
           "Enter must retain the edited title");

    const auto mode_switch = find_element(ui, "mode-switch");
    const auto mode_switch_position = mode_switch.absolute_position();
    const auto mode_switch_size = mode_switch.size();
    const auto drag_space = find_element(ui, "titlebar-blank-space");
    const auto drag_position = drag_space.absolute_position();
    expect(drag_position.x - (mode_switch_position.x + mode_switch_size.width) <= 1.0F,
           "the drag region must start immediately after the last header control");
    click_element(ui, "titlebar-blank-space", 0.02F);
    expect(ui->get_drag_count() == 1,
           "the title-adjacent blank region must start native window dragging");

    // The composer model menu must cap at six visible rows (plus header) and
    // still reach every entry through wheel scrolling.
    {
        auto choices = std::make_shared<slint::VectorModel<ModelChoice>>();
        for (int i = 0; i < 8; ++i) {
            ModelChoice choice;
            const auto index = std::to_string(i);
            choice.name = slint::SharedString("prov-" + index);
            choice.model = slint::SharedString("model-" + index);
            choice.label = slint::SharedString("prov-" + index + " · model-" + index);
            choices->push_back(std::move(choice));
        }
        ui->set_test_model_choices(choices);
        ui->set_test_model_menu_open(true);
        slint::platform::update_timers_and_animations();

        const auto popup = find_element(ui, "model-menu-popup");
        const auto popup_position = popup.absolute_position();
        const auto popup_size = popup.size();
        expect(std::abs(popup_size.height - 214.0F) < 0.5F,
               "the model menu must show at most six rows instead of growing with the choice count");
        // Row index 1 (second visible row) — regression guard for a dead-row bug.
        send_mouse_click(ui.operator->(), popup_position.x + popup_size.width * 0.5F,
                         popup_position.y + 75.0F);
        slint::platform::update_timers_and_animations();
        expect(as_string(ui->get_test_model_name()) == "model-1",
               "clicking the second visible model row must select the second choice");
        expect(!ui->get_test_model_menu_open(), "picking a model must close the menu");

        ui->set_test_model_menu_open(true);
        slint::platform::update_timers_and_animations();
        send_mouse_click(ui.operator->(), popup_position.x + popup_size.width * 0.5F,
                         popup_position.y + 45.0F);
        slint::platform::update_timers_and_animations();
        expect(as_string(ui->get_test_model_name()) == "model-0",
               "clicking the first visible model row must select the first choice");
        expect(!ui->get_test_model_menu_open(), "picking a model must close the menu");

        ui->set_test_model_menu_open(true);
        slint::platform::update_timers_and_animations();
        ui->window().dispatch_pointer_scroll_event(
                slint::LogicalPosition({ popup_position.x + popup_size.width * 0.5F,
                                         popup_position.y + popup_size.height * 0.5F }),
                0.0F, -240.0F);
        advance_time(300);
        slint::platform::update_timers_and_animations();
        send_mouse_click(ui.operator->(), popup_position.x + popup_size.width * 0.5F,
                         popup_position.y + 195.0F);
        slint::platform::update_timers_and_animations();
        expect(as_string(ui->get_test_model_name()) == "model-7",
               "wheel scrolling must expose the trailing model choices for selection");
        ui->set_test_model_menu_open(false);
    }

    // The settings model dropdown must also cap at six rows, keep its options
    // clickable, and scroll to the trailing entries.
    {
        std::vector<slint::SharedString> profiles;
        std::vector<slint::SharedString> models;
        for (int i = 0; i < 8; ++i) {
            profiles.push_back(slint::SharedString("profile-" + std::to_string(i)));
            models.push_back(slint::SharedString("m" + std::to_string(i)));
        }
        ui->set_test_agent_options(
                std::make_shared<slint::VectorModel<slint::SharedString>>(profiles));
        ui->set_test_agent_names(
                std::make_shared<slint::VectorModel<slint::SharedString>>(profiles));
        ui->set_test_agent_models(
                std::make_shared<slint::VectorModel<slint::SharedString>>(models));
        ui->set_test_setting_name(slint::SharedString("unset"));
        ui->set_test_setting_main_model(slint::SharedString("m0"));
        ui->set_test_model_name(slint::SharedString("m0"));
        ui->set_test_settings_open(true);
        ui->set_test_settings_page(1);
        ui->set_test_chat_empty(false);
        slint::platform::update_timers_and_animations();

        click_element(ui, "main-model-select");
        slint::platform::update_timers_and_animations();
        const auto box = find_element(ui, "main-model-select");
        const auto box_position = box.absolute_position();
        const auto box_size = box.size();
        // The dropdown hangs 6px below the box; rows are 30px tall with 4px
        // padding at the top of the list.
        const float dropdown_top = box_position.y + box_size.height + 6.0F;
        const float dropdown_x = box_position.x + box_size.width * 0.5F;

        // The row that previously sat underneath the following settings rows
        // (推理强度) must be clickable now that the dropdown is a popup layer.
        click_element(ui, "main-model-select");
        slint::platform::update_timers_and_animations();
        // If the popup opened, this outside click is consumed by its
        // close-on-click-outside policy and the settings stay open. Otherwise
        // the click dismisses the whole settings overlay.
        send_mouse_click(ui.operator->(), 30.0F, 30.0F);
        slint::platform::update_timers_and_animations();
        std::cerr << "probe outside-click: settings-open=" << ui->get_test_settings_open()
                  << '\n';
        ui->set_test_settings_open(true);
        slint::platform::update_timers_and_animations();
        (void)ElementHandle::visit_elements(
                ui, [](const ElementHandle &element) -> std::optional<ElementHandle> {
                    const auto pos = element.absolute_position();
                    const auto size = element.size();
                    if (pos.x > 300.0F && pos.x < 900.0F && pos.y > 300.0F && pos.y < 620.0F) {
                        std::cerr << "elem type="
                                  << (element.type_name() ? as_string(*element.type_name())
                                                          : std::string("-"))
                                  << " id="
                                  << (element.id() ? as_string(*element.id()) : std::string("-"))
                                  << " at (" << pos.x << "," << pos.y << ") " << size.width
                                  << "x" << size.height << '\n';
                    }
                    return std::nullopt;
                });
        for (int probe_row : {0, 2, 1}) {
            click_element(ui, "main-model-select");
            slint::platform::update_timers_and_animations();
            send_mouse_click(ui.operator->(), dropdown_x, dropdown_top + 4.0F + 30.0F * probe_row + 15.0F);
            slint::platform::update_timers_and_animations();
            std::cerr << "probe row " << probe_row << ": name="
                      << as_string(ui->get_test_setting_name())
                      << " model=" << as_string(ui->get_test_setting_main_model()) << '\n';
        }
        expect(as_string(ui->get_test_setting_main_model()) == "m1",
               "clicking the dropdown's second row must apply that profile's model");

        click_element(ui, "main-model-select");
        slint::platform::update_timers_and_animations();
        ui->window().dispatch_pointer_scroll_event(
                slint::LogicalPosition({ dropdown_x, dropdown_top + 94.0F }), 0.0F, -240.0F);
        advance_time(300);
        slint::platform::update_timers_and_animations();
        send_mouse_click(ui.operator->(), dropdown_x, dropdown_top + 165.0F);
        slint::platform::update_timers_and_animations();
        expect(as_string(ui->get_test_setting_main_model()) == "m7",
               "wheel scrolling must expose the trailing dropdown options");
        ui->set_test_settings_open(false);
    }

    std::cout << "title edit UI interactions passed\n";
    return EXIT_SUCCESS;
}
