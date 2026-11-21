# Extension compatibility matrix

Generated from `tests/extensions/matrix.json` by `pnpm exec nx run extensions-suite:matrix`. Do not edit by hand.

**Status: not run.** No engine results were supplied, so every observed result below is "not run". The expected statuses are the plan from `docs/planning/sdk/extensions.md`, not test results. `pnpm test:release` runs the engine's extension tests and regenerates this file with the results.

Expected status: `supported` behaves as in Chrome. `degraded` exists and differs. `absent` is missing from the extension's `chrome` object. `gap` is a known missing or unverified capability. Observed: `passed` when every engine test listed for the row passed, `failed` when any failed, `not run` otherwise.

## T-EXT-01 assertions

| Row | Expected | Observed | Evidence |
|---|---|---|---|
| The worker starts and every content script acknowledges | supported | not run | `engine_ext_probe_worker_and_content_ack` |
| tabs.query({active, currentWindow}) returns exactly the focused page | supported | not run | `engine_ext_probe_active_tab_is_focused_page` |
| Focusing a page fires tabs.onActivated for it | supported | not run | `engine_ext_probe_focus_fires_on_activated` |
| Moving a page to a second window keeps its tab id and fires detach then attach | supported | not run | `engine_ext_probe_move_keeps_tab_id_and_events` |
| An action click grants activeTab to the focused page only, and navigating revokes it | supported | not run | `engine_ext_probe_action_grants_active_tab_to_one_page` |
| The popup opens anchored to an arbitrary rectangle, reports the same active tab and closes on blur | supported | not run | `engine_ext_probe_popup_reports_active_tab`<br>`engine_ext_ui_popup_closes_on_blur_and_the_request_is_withdrawn`<br>`engine_ext_ui_the_application_closes_a_popup_and_navigation_closes_it` |
| tabs.sendMessage reaches the content script of both pages, and of the moved one | supported | not run | `engine_ext_probe_messages_reach_both_pages` |
| The context-menu item is in the menu model and its click names the right tab | supported | not run | `engine_ext_probe_context_menu_names_tab` |
| sidePanel.open works or fails with an explicit error | supported | not run | `engine_ext_probe_side_panel_open_result` |
| windows.getAll does not list the parking context | supported | not run | `engine_ext_probe_parking_context_hidden` |

## Extension APIs that need the host

| Row | Expected | Observed | Evidence |
|---|---|---|---|
| `action` | supported | not run | `engine_ext_ui_actions_report_state_and_badge_changes`<br>`engine_ext_ui_activation_targets_the_focused_page`<br>`engine_ext_ui_activation_without_a_gesture_fails`<br>`engine_ext_ui_action_open_popup_opens_a_popup` |
| `sidePanel` | supported | not run | `engine_ext_panel_side_panel_open_shows_the_default_panel_at_the_trailing_edge`<br>`engine_ext_panel_side_panel_open_reaches_an_application_container` |
| `contextMenus` | supported | not run | `engine_ext_ui_a_context_menu_item_is_listed_performed_and_grants_active_tab` |
| `commands` | degraded | not run | `engine_ext_ui_a_declared_command_fires_oncommand_for_the_page`<br>`engine_ext_probe_api_presence` |
| `tabs.create` | supported | not run | `engine_ext_probe_new_window_requests` |
| `windows.create` | supported | not run | `engine_ext_probe_new_window_requests` |
| `tabs.update` | degraded | not run | `engine_ext_probe_tabs_update_active_request` |
| `tabs.highlight` | degraded | not run | `engine_ext_probe_tabs_update_active_request` |
| `tabs.move` | degraded | not run | `engine_ext_probe_move_keeps_tab_id_and_events` |
| `tabs.group` | degraded | not run | `engine_ext_probe_api_presence` |
| `tabGroups` | degraded | not run | `engine_ext_probe_api_presence` |
| `tabs.query` | degraded | not run | `engine_ext_probe_active_tab_is_focused_page`<br>`engine_ext_probe_offscreen_pages_in_query` |
| `tabs.discard` | supported | not run | `engine_ext_probe_tab_requests_reach_delegate` |
| `tabs.duplicate` | supported | not run | `engine_ext_probe_tab_requests_reach_delegate` |
| `tabs.remove` | supported | not run | `engine_ext_probe_tab_requests_reach_delegate` |
| `windows.update` | supported | not run | `engine_ext_probe_window_update_request` |
| `sessions` | gap | not run | `engine_ext_probe_sessions_empty` |
| `omnibox` | gap | not run | `engine_ext_probe_api_presence` |
| `notifications` | supported | not run | `engine_ext_ui_notifications` |
| `downloads` | supported | not run | `engine_ext_ui_downloads` |
| `downloads.setShelfEnabled` | degraded | not run | `engine_ext_ui_downloads`<br>`engine_ext_probe_api_presence` |
| `downloads.setUiOptions` | degraded | not run | `engine_ext_ui_downloads`<br>`engine_ext_probe_api_presence` |
| `identity.launchWebAuthFlow` | supported | not run | `engine_ext_ui_web_auth_flow_popup` |
| `identity.getAuthToken` | absent | not run | `engine_ext_probe_api_presence` |
| `identity.getProfileUserInfo` | absent | not run | `engine_ext_probe_api_presence` |
| `debugger` | supported | not run | `engine_ext_ui_debugger_notice` |
| `desktopCapture` | supported | not run | `engine_ext_ui_screen_capture_request` |
| `tabCapture` | supported | not run | `engine_ext_ui_screen_capture_request` |
| `permissions.request` | supported | not run | `engine_ext_install_optional_permissions_prompt` |
| `management` | supported | not run | `engine_ext_install_uninstall_raises_the_uninstall_request` |
| `bookmarks` | supported | not run | `engine_ext_probe_api_presence` |
| `history` | supported | not run | `engine_ext_probe_api_presence` |
| `topSites` | supported | not run | `engine_ext_probe_api_presence` |
| `cookies` | supported | not run | `engine_ext_probe_api_presence` |
| `browsingData` | supported | not run | `engine_ext_probe_api_presence` |
| `alarms` | supported | not run | `engine_ext_probe_api_presence` |
| `i18n` | supported | not run | `engine_ext_probe_api_presence` |
| `webNavigation` | supported | not run | `engine_ext_probe_api_presence` |
| `offscreen` | supported | not run | `engine_ext_probe_api_presence` |
| `storage` | supported | not run | `engine_ext_probe_api_presence`<br>`engine_ext_probe_passwords_fills_login_form` |
| `scripting` | supported | not run | `engine_ext_probe_api_presence`<br>`engine_ext_probe_action_grants_active_tab_to_one_page` |
| `webRequest` | supported | not run | `engine_ext_probe_api_presence` |
| `declarativeNetRequest` | supported | not run | `engine_ext_probe_api_presence`<br>`engine_ext_probe_blocker_blocks_request` |
| `runtime` | supported | not run | `engine_ext_probe_api_presence`<br>`engine_ext_probe_worker_and_content_ack` |
| `search` | degraded | not run | `engine_ext_probe_api_presence` |
| `tts` | supported | not run | `engine_ext_probe_api_presence` |
| `ttsEngine` | gap | not run | `engine_ext_probe_api_presence` |
| `privacy` | supported | not run | `engine_ext_probe_api_presence` |
| `proxy` | supported | not run | `engine_ext_probe_api_presence` |
| `contentSettings` | supported | not run | `engine_ext_probe_api_presence` |
| `fontSettings` | supported | not run | `engine_ext_probe_api_presence` |
| `webstore` | absent | not run | `engine_ext_probe_api_presence` |
| `webstorePrivate` | absent | not run | `engine_ext_probe_api_presence` |
| `enterprise.*` | absent | not run | `engine_ext_probe_api_presence` |
| `platformKeys` | absent | not run | `engine_ext_probe_api_presence` |
| `loginState` | absent | not run | `engine_ext_probe_api_presence` |

## UI surfaces

| Row | Expected | Observed | Evidence |
|---|---|---|---|
| Actions in the default extensions menu | supported | not run | `engine_ext_ui_popup_opens_from_the_default_menu_with_no_handler`<br>`engine_ext_ui_a_window_with_no_anchor_gets_a_title_bar_button`<br>`engine_ext_ui_a_registered_anchor_replaces_the_title_bar_button` |
| Actions drawn by the application from ActionState | supported | not run | `engine_ext_ui_actions_report_state_and_badge_changes`<br>`engine_ext_ui_activation_targets_the_focused_page`<br>`engine_ext_ui_activation_without_a_gesture_fails` |
| Popup in the default transient window | supported | not run | `engine_ext_ui_popup_opens_from_the_default_menu_with_no_handler` |
| Popup placed by the application | supported | not run | `engine_ext_ui_popup_with_a_handler_gets_the_secure_view_and_sizes` |
| Side panel at the trailing edge of the page's window | supported | not run | `engine_ext_panel_side_panel_open_shows_the_default_panel_at_the_trailing_edge` |
| Side panel embedded by the application | supported | not run | `engine_ext_panel_side_panel_open_reaches_an_application_container`<br>`engine_ext_panel_an_extension_closing_its_panel_withdraws_the_request` |
| Page-scoped side panel while two pages are visible | gap | not run | `engine_ext_panel_a_page_panel_hides_and_shows_with_the_active_page_and_a_second_panel_replaces_it` |
| Extension items in the context-menu request | supported | not run | `engine_ext_ui_a_context_menu_item_is_listed_performed_and_grants_active_tab` |
| Options page as a new-window request | supported | not run | `engine_ext_ui_openoptions_raises_a_new_page_request_with_the_extension`<br>`engine_ext_ui_chrome_runtime_openoptionspage_raises_the_same_request` |
| DevTools extension panels in the default front end | supported | not run | `engine_ext_panel_a_devtools_page_adds_its_panel_to_undocked_devtools` |
| DevTools extension panels with a replaced front end | gap | not run | `engine_ext_panel_devtools_replaced_frontend_gap` |
| An extension that needs a disabled surface is refused | supported | not run | `engine_ext_install_an_incompatible_manifest_is_refused_with_the_finding`<br>`engine_ext_install_allow_all_installs_it_and_the_disabled_apis_are_absent` |
| Install, permission and uninstall prompts | supported | not run | `engine_ext_install_a_grant_without_a_recent_gesture_installs_nothing`<br>`engine_ext_install_denying_the_install_cancels_it`<br>`engine_ext_install_with_no_handler_the_default_alert_lists_the_warnings`<br>`engine_ext_install_uninstall_raises_the_uninstall_request` |

## Installation sources and policy

| Row | Expected | Observed | Evidence |
|---|---|---|---|
| Install from an unpacked directory | supported | not run | `engine_ext_install_a_handler_grants_an_unpacked_install_with_a_gesture` |
| Install from a local CRX | supported | not run | `engine_ext_install_a_crx_built_at_test_time_installs_with_the_prompt` |
| Install from a self-hosted update URL | supported | not run | `engine_ext_install_a_self_hosted_update_url_installs_and_then_updates` |
| Install from a named store | supported | not run | `engine_ext_install_a_store_that_is_not_configured_is_refused`<br>`engine_ext_install_the_store_button_says_add_to_the_browser_name`<br>`engine_ext_install_the_store_button_is_left_alone_when_the_store_is_off` |
| Bundled extensions | gap | not run | `engine_ext_install_bundled_not_read` |
| A source that is switched off is refused with a reason | supported | not run | `engine_ext_install_unpacked_off_is_refused_naming_the_key`<br>`engine_ext_install_crx_off_is_refused_naming_the_key`<br>`engine_ext_install_an_update_url_outside_the_patterns_is_refused` |
| An update applies; one that adds permissions disables until approved | supported | not run | `engine_ext_install_an_update_that_adds_permissions_is_disabled_until_approved`<br>`engine_ext_install_an_update_that_becomes_incompatible_is_refused` |
| Pre-install compatibility report | supported | not run | `engine_ext_install_an_incompatible_manifest_is_refused_with_the_finding` |

## Private profiles

| Row | Expected | Observed | Evidence |
|---|---|---|---|
| Manifest incognito mode spanning | supported | not run | `engine_ext_probe_incognito_spanning` |
| Manifest incognito mode split | supported | not run | `engine_ext_probe_incognito_split` |
| allow_in_private = never | supported | not run | `engine_ext_install_allowed_in_private_is_refused_when_private_is_never` |

## Host mapping

| Row | Expected | Observed | Evidence |
|---|---|---|---|
| Off-screen pages in tabs.query and windows.getAll | degraded | not run | `engine_ext_probe_offscreen_pages_in_query` |

## Representative extensions

| Row | Expected | Observed | Evidence |
|---|---|---|---|
| Content blocker (declarativeNetRequest static rules) | supported | not run | `engine_ext_probe_blocker_blocks_request` |
| Password manager (popup, content scripts on all sites, storage, context menu) | supported | not run | `engine_ext_probe_passwords_fills_login_form` |
| Developer tool (devtools_page, panel, inspectedWindow.eval) | supported | not run | `engine_ext_panel_devtool_inspected_window_eval` |
| uBlock Origin Lite | supported | not run | `engine_ext_install_real_ublock_origin_lite` |
| Bitwarden | supported | not run | `engine_ext_install_real_bitwarden` |
| React Developer Tools | supported | not run | `engine_ext_install_real_react_developer_tools` |

## Known differences from Chrome

Every row whose expected status is not `supported`.

### Degraded: present, but behaves differently from Chrome

| Row | What differs | Observed |
|---|---|---|
| `commands` | The application owns shortcuts. Without application support the default menu lists the commands and nothing is bound. | not run |
| `tabs.update` | Setting active raises an activation request to the window delegate. The application may ignore it and the call then resolves with no change. | not run |
| `tabs.highlight` | Raises an activation request to the window delegate, which may ignore it. | not run |
| `tabs.move` | BDK has no tab order. A move between windows moves the page's view only if the application's delegate does it. | not run |
| `tabs.group` | Groups are made in the engine's tab model of the page's host context, but BDK has no group concept and the application shows none. | not run |
| `tabGroups` | The namespace works on the engine's tab model, but the application shows no groups (inspection reports api.tabGroups.notShown). | not run |
| `tabs.query` | Pages attached to a window are that window's tabs. Off-screen pages follow decision D1 and the parking context is hidden. | not run |
| `downloads.setShelfEnabled` | Accepted and passed to the presenter as a hint only. | not run |
| `downloads.setUiOptions` | Accepted and passed to the presenter as a hint only. | not run |
| `search` | Uses the default engine from the search service and is absent without one. | not run |
| Off-screen pages in tabs.query and windows.getAll | Follow decision D1; what extensions observe depends on the mapping (risk R9). | not run |

### Gap: Chrome has it, BDK does not yet, or it is unverified

| Row | What differs | Observed |
|---|---|---|
| `sessions` | BDK has no session history: getRecentlyClosed returns an empty list and restore rejects. | not run |
| `omnibox` | Needs the search.suggestions service (addition S1). Until then keywords reach no address bar. | not run |
| `ttsEngine` | Unverified: no fixture registers a speech engine, so only the absence of the namespace is observed. | not run |
| Page-scoped side panel while two pages are visible | Unverified, recorded for risk R9: a page-scoped panel is meant to belong to the focused page. | not run |
| DevTools extension panels with a replaced front end | Panels are not loaded when [devtools] frontend is replaced (extensions.md 4.8). | not run |
| Bundled extensions | [extensions.bundled] is not in the schema yet, so nothing is read. | not run |

### Absent: not in the extension's chrome object by design

| Row | What differs | Observed |
|---|---|---|
| `identity.getAuthToken` | No Google sign-in exists. | not run |
| `identity.getProfileUserInfo` | No Google sign-in exists. | not run |
| `webstore` | Only with a store source (decision D4); not configured in the suite. | not run |
| `webstorePrivate` | Reserved for the store's own pages; only with a store source (decision D4). | not run |
| `enterprise.*` | Enterprise and ChromeOS APIs are absent. | not run |
| `platformKeys` | ChromeOS only. | not run |
| `loginState` | ChromeOS only. | not run |
