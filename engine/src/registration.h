#pragma once

#include <skyggn/skyggn.h>

#include <string_view>

namespace skyggn {

bool is_registered(skyggn_scope scope);
HRESULT install(skyggn_scope scope);
HRESULT register_server(skyggn_scope scope);
HRESULT unregister_server(skyggn_scope scope);
HRESULT set_handled(skyggn_scope scope, std::wstring_view extension, bool handled);
bool is_handled(skyggn_scope scope, std::wstring_view extension);
skyggn_handler effective_handler(std::wstring_view extension);
// finds thumbnail registrations whose program is gone; deletes them when `remove` is set
HRESULT find_dead(skyggn_scope scope, bool remove, UINT* count);
// windows reads some types' details only with its own handler (matroska among them, where it gives
// little more than the length). take sets those entries aside, for the types skyggn reads details
// for, so skyggn's handler serves them; giving back puts windows' entries back. needs admin.
HRESULT take_system_details(bool take);
// whether skyggn reads those types' details now: taken, and not put back since by a windows update
bool system_details_taken();

}  // namespace skyggn
