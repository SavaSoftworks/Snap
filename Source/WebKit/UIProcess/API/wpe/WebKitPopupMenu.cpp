/*
 * Copyright (C) 2020 Igalia S.L.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "WebKitPopupMenu.h"

#include "APIViewClient.h"
#include "WPEWebView.h"
#include "WebKitOptionMenuPrivate.h"
#include "WebKitWebViewClient.h"

#if ENABLE(WPE_PLATFORM)
#include <wpe/wpe-platform.h>
#endif

namespace WebKit {
using namespace WebCore;

Ref<WebKitPopupMenu> WebKitPopupMenu::create(WKWPE::View& view, WebPopupMenuProxy::Client& client)
{
    return adoptRef(*new WebKitPopupMenu(view, client));
}

WebKitPopupMenu::WebKitPopupMenu(WKWPE::View& view, WebPopupMenuProxy::Client& client)
    : WebPopupMenuProxy(client)
    , m_view(view)
{
}

static void menuCloseCallback(WebKitPopupMenu* popupMenu)
{
    popupMenu->activateItem(std::nullopt);
}

void WebKitPopupMenu::showPopupMenu(const IntRect& rect, TextDirection direction, double pageScaleFactor, const Vector<WebPopupItem>& items, const PlatformPopupMenuData& platformData, int32_t selectedIndex)
{
    GRefPtr<WebKitOptionMenu> menu;
    if (m_view.client().isGLibBasedAPI())
        menu = static_cast<WebKitWebViewClient&>(m_view.client()).showOptionMenu(*this, rect, items, selectedIndex);
#if ENABLE(WPE_PLATFORM)
    else if (auto* wpeView = m_view.wpeView()) {
        const guint signal = g_signal_lookup("eden-option-menu-v1", WPE_TYPE_VIEW);
        if (signal && g_signal_has_handler_pending(wpeView, signal, 0, TRUE)) {
            menu = adoptGRef(webkitOptionMenuCreate(*this, items, selectedIndex));
            g_signal_emit_by_name(wpeView, "eden-option-menu-v1", menu.get(), rect.x(), rect.y(), rect.width(), rect.height());
        }
    }
#endif
    if (menu) {
        m_menu = WTF::move(menu);
        g_signal_connect_swapped(m_menu.get(), "close", G_CALLBACK(menuCloseCallback), this);
        return;
    }

    if (CheckedPtr checkedClient = client())
        checkedClient->failedToShowPopupMenu();
}

void WebKitPopupMenu::hidePopupMenu()
{
    if (m_menu) {
        g_signal_handlers_disconnect_matched(m_menu.get(), G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, this);
        webkit_option_menu_close(m_menu.get());
    }
}

void WebKitPopupMenu::cancelTracking()
{
    hidePopupMenu();
    m_menu = nullptr;
}

void WebKitPopupMenu::selectItem(unsigned itemIndex)
{
    if (CheckedPtr client = this->client())
        client->setTextFromItemForPopupMenu(this, itemIndex);
    m_selectedItem = itemIndex;
}

void WebKitPopupMenu::activateItem(std::optional<unsigned> itemIndex)
{
    if (CheckedPtr client = this->client())
        client->valueChangedForPopupMenu(this, itemIndex.value_or(m_selectedItem.value_or(-1)));
    if (m_menu) {
        g_signal_handlers_disconnect_matched(m_menu.get(), G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, this);
        m_menu = nullptr;
    }
}

} // namespace WebKit
