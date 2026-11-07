/*
 * Copyright (C) 2007, 2008 Apple Inc. All rights reserved.
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
#include "JSHTMLCollection.h"

#include "DOMWrapperWorld.h"
#include "Document.h"
#include "DocumentQuirks.h"
#include "Element.h"
#include "HTMLCollectionInlines.h"
#include "JSDOMAbstractOperations.h"
#include "JSDOMBinding.h"
#include "JSDOMConvertInterface.h"
#include "JSDOMConvertNullable.h"
#include "JSDOMExceptionHandling.h"
#include "JSDOMGlobalObject.h"
#include "JSElement.h"
#include "ScriptExecutionContext.h"
#include <JavaScriptCore/IndexingType.h>

namespace WebCore {
using namespace JSC;

static bool getHTMLCollectionOwnProperty(JSObject* object, JSGlobalObject* lexicalGlobalObject, PropertyName propertyName, PropertySlot& slot, bool ignoreNamedProperties)
{
    auto throwScope = DECLARE_THROW_SCOPE(JSC::getVM(lexicalGlobalObject));
    auto* thisObject = uncheckedDowncast<JSHTMLCollection>(object);
    ASSERT_GC_OBJECT_INHERITS(thisObject, JSHTMLCollection::info());
    if (auto index = parseIndex(propertyName)) {
        if (auto item = thisObject->wrapped().item(index.value()); !!item) [[likely]] {
            auto value = toJS<IDLNullable<IDLInterface<Element>>>(*lexicalGlobalObject, *thisObject->realm(), throwScope, IDLNullable<IDLInterface<Element>>::extractValueFromNullable(WTF::move(item)));
            RETURN_IF_EXCEPTION(throwScope, false);
            slot.setValue(thisObject, static_cast<unsigned>(JSC::PropertyAttribute::ReadOnly), value);
            return true;
        }
        if (!hasIndexedProperties(thisObject->indexingType()))
            return false;
        return WebCore::getOwnPropertySlot(object, lexicalGlobalObject, propertyName, slot);
    }
    if (!ignoreNamedProperties) {
        using GetterIDLType = IDLInterface<Element>;
        auto getterFunctor = visibleNamedPropertyItemAccessorFunctor<GetterIDLType, JSHTMLCollection>([](JSHTMLCollection& thisObject, PropertyName propertyName) -> decltype(auto) {
            return thisObject.wrapped().namedItem(propertyNameToAtomString(propertyName));
        });
        if (auto namedProperty = accessVisibleNamedProperty<LegacyOverrideBuiltIns::No>(*lexicalGlobalObject, *thisObject, propertyName, getterFunctor)) {
            auto value = toJS<IDLNullable<IDLInterface<Element>>>(*lexicalGlobalObject, *thisObject->realm(), throwScope, WTF::move(namedProperty.value()));
            RETURN_IF_EXCEPTION(throwScope, false);
            slot.setValue(thisObject, JSC::PropertyAttribute::ReadOnly | JSC::PropertyAttribute::DontEnum, value);
            return true;
        }
    }
    return WebCore::getOwnPropertySlot(object, lexicalGlobalObject, propertyName, slot);
}

bool JSHTMLCollection::getOwnPropertySlot(JSObject* object, JSGlobalObject* lexicalGlobalObject, PropertyName propertyName, PropertySlot& slot)
{
    bool ignoreNamedProperties = false;
    return getHTMLCollectionOwnProperty(object, lexicalGlobalObject, propertyName, slot, ignoreNamedProperties);
}

bool JSHTMLCollection::getOwnPropertySlotByIndex(JSObject* object, JSGlobalObject* lexicalGlobalObject, unsigned index, PropertySlot& slot)
{
    SUPPRESS_UNCOUNTED_LOCAL auto& vm = JSC::getVM(lexicalGlobalObject);
    auto* thisObject = uncheckedDowncast<JSHTMLCollection>(object);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    if (index <= MAX_ARRAY_INDEX) [[likely]] {
        auto throwScope = DECLARE_THROW_SCOPE(vm);
        if (auto item = thisObject->wrapped().item(index); !!item) [[likely]] {
            auto value = toJS<IDLNullable<IDLInterface<Element>>>(*lexicalGlobalObject, *thisObject->realm(), throwScope, IDLNullable<IDLInterface<Element>>::extractValueFromNullable(WTF::move(item)));
            RETURN_IF_EXCEPTION(throwScope, false);
            slot.setValue(thisObject, static_cast<unsigned>(JSC::PropertyAttribute::ReadOnly), value);
            return true;
        }
        if (!hasIndexedProperties(thisObject->indexingType()))
            return false;
        return JSObject::getOwnPropertySlotByIndex(object, lexicalGlobalObject, index, slot);
    }
    auto propertyName = Identifier::from(vm, index);
    auto throwScope = DECLARE_THROW_SCOPE(vm);
    using GetterIDLType = IDLInterface<Element>;
    auto getterFunctor = visibleNamedPropertyItemAccessorFunctor<GetterIDLType, JSHTMLCollection>([](JSHTMLCollection& thisObject, PropertyName propertyName) -> decltype(auto) {
        return thisObject.wrapped().namedItem(propertyNameToAtomString(propertyName));
    });
    if (auto namedProperty = accessVisibleNamedProperty<LegacyOverrideBuiltIns::No>(*lexicalGlobalObject, *thisObject, propertyName, getterFunctor)) {
        auto value = toJS<IDLNullable<IDLInterface<Element>>>(*lexicalGlobalObject, *thisObject->realm(), throwScope, WTF::move(namedProperty.value()));
        RETURN_IF_EXCEPTION(throwScope, false);
        slot.setValue(thisObject, JSC::PropertyAttribute::ReadOnly | JSC::PropertyAttribute::DontEnum, value);
        return true;
    }
    return JSObject::getOwnPropertySlotByIndex(object, lexicalGlobalObject, index, slot);
}

bool JSHTMLCollection::put(JSCell* cell, JSGlobalObject* lexicalGlobalObject, PropertyName propertyName, JSValue value, PutPropertySlot& putPropertySlot)
{
    auto* thisObject = uncheckedDowncast<JSHTMLCollection>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());

    if (thisObject != putPropertySlot.thisValue()) [[unlikely]]
        return JSObject::put(thisObject, lexicalGlobalObject, propertyName, value, putPropertySlot);

    if (auto* document = dynamicDowncast<Document>(dynamicDowncast<JSDOMGlobalObject>(lexicalGlobalObject)->scriptExecutionContext())) {
        if (document->quirks().needsConfigurableIndexedPropertiesQuirk()) [[unlikely]]
            return JSObject::put(thisObject, lexicalGlobalObject, propertyName, value, putPropertySlot);
    }

    auto throwScope = DECLARE_THROW_SCOPE(lexicalGlobalObject->vm());

    throwScope.assertNoException();
    PropertyDescriptor ownDescriptor;
    PropertySlot slot(thisObject, PropertySlot::InternalMethodType::GetOwnProperty);
    bool ignoreNamedProperties = true;
    bool hasOwnProperty = getHTMLCollectionOwnProperty(thisObject, lexicalGlobalObject, propertyName, slot, ignoreNamedProperties);
    RETURN_IF_EXCEPTION(throwScope, false);
    if (hasOwnProperty) {
        ownDescriptor.setPropertySlot(lexicalGlobalObject, propertyName, slot);
        RETURN_IF_EXCEPTION(throwScope, false);
    }
    RELEASE_AND_RETURN(throwScope, ordinarySetWithOwnDescriptor(lexicalGlobalObject, thisObject, propertyName, value, putPropertySlot.thisValue(), WTF::move(ownDescriptor), putPropertySlot.isStrictMode()));
}

bool JSHTMLCollection::putByIndex(JSCell* cell, JSGlobalObject* lexicalGlobalObject, unsigned index, JSValue value, bool shouldThrow)
{
    if (auto* document = dynamicDowncast<Document>(dynamicDowncast<JSDOMGlobalObject>(lexicalGlobalObject)->scriptExecutionContext())) {
        if (document->quirks().needsConfigurableIndexedPropertiesQuirk()) [[unlikely]]
            return JSObject::putByIndex(cell, lexicalGlobalObject, index, value, shouldThrow);
    }

    auto* thisObject = uncheckedDowncast<JSHTMLCollection>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());

    SUPPRESS_UNCOUNTED_LOCAL auto& vm = JSC::getVM(lexicalGlobalObject);
    auto throwScope = DECLARE_THROW_SCOPE(vm);

    throwScope.assertNoException();
    auto propertyName = Identifier::from(vm, index);
    PutPropertySlot putPropertySlot(thisObject, shouldThrow);
    RELEASE_AND_RETURN(throwScope, ordinarySetSlow(lexicalGlobalObject, thisObject, propertyName, value, putPropertySlot.thisValue(), shouldThrow));
}

} // namespace WebCore
