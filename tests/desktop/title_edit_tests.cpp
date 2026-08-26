#include "title_edit_harness.h"

#include <slint-platform.h>
#include <private/slint_tests_helpers.h>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

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

    const auto edit_button = find_element(ui, "edit-title-button");
    const auto edit_position = edit_button.absolute_position();
    const auto edit_size = edit_button.size();
    const auto drag_space = find_element(ui, "titlebar-blank-space");
    const auto drag_position = drag_space.absolute_position();
    expect(drag_position.x - (edit_position.x + edit_size.width) <= 1.0F,
           "the drag region must start immediately after the title controls");
    click_element(ui, "titlebar-blank-space", 0.02F);
    expect(ui->get_drag_count() == 1,
           "the title-adjacent blank region must start native window dragging");

    std::cout << "title edit UI interactions passed\n";
    return EXIT_SUCCESS;
}
