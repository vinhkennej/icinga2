/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012-2015 Icinga Development Team (http://www.icinga.org)    *
 *                                                                            *
 * This program is free software; you can redistribute it and/or              *
 * modify it under the terms of the GNU General Public License                *
 * as published by the Free Software Foundation; either version 2             *
 * of the License, or (at your option) any later version.                     *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program; if not, write to the Free Software Foundation     *
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.             *
 ******************************************************************************/

#include "config/configitem.hpp"
#include "config/configcompilercontext.hpp"
#include "config/applyrule.hpp"
#include "config/objectrule.hpp"
#include "base/application.hpp"
#include "base/configtype.hpp"
#include "base/objectlock.hpp"
#include "base/convert.hpp"
#include "base/logger.hpp"
#include "base/debug.hpp"
#include "base/workqueue.hpp"
#include "base/exception.hpp"
#include "base/stdiostream.hpp"
#include "base/netstring.hpp"
#include "base/serializer.hpp"
#include "base/json.hpp"
#include "base/exception.hpp"
#include "base/function.hpp"
#include <sstream>
#include <fstream>
#include <boost/foreach.hpp>

using namespace icinga;

boost::mutex ConfigItem::m_Mutex;
ConfigItem::TypeMap ConfigItem::m_Items;
ConfigItem::ItemList ConfigItem::m_UnnamedItems;
ConfigItem::ItemList ConfigItem::m_CommittedItems;

REGISTER_SCRIPTFUNCTION(commit_objects, &ConfigItem::CommitAndActivate);

/**
 * Constructor for the ConfigItem class.
 *
 * @param type The object type.
 * @param name The name of the item.
 * @param unit The unit of the item.
 * @param abstract Whether the item is a template.
 * @param exprl Expression list for the item.
 * @param debuginfo Debug information.
 */
ConfigItem::ConfigItem(const String& type, const String& name,
    bool abstract, const boost::shared_ptr<Expression>& exprl,
    const boost::shared_ptr<Expression>& filter,
    const DebugInfo& debuginfo, const Dictionary::Ptr& scope,
    const String& zone, const String& package)
	: m_Type(type), m_Name(name), m_Abstract(abstract),
	  m_Expression(exprl), m_Filter(filter),
	  m_DebugInfo(debuginfo), m_Scope(scope), m_Zone(zone),
	  m_Package(package)
{
}

/**
 * Retrieves the type of the configuration item.
 *
 * @returns The type.
 */
String ConfigItem::GetType(void) const
{
	return m_Type;
}

/**
 * Retrieves the name of the configuration item.
 *
 * @returns The name.
 */
String ConfigItem::GetName(void) const
{
	return m_Name;
}

/**
 * Checks whether the item is abstract.
 *
 * @returns true if the item is abstract, false otherwise.
 */
bool ConfigItem::IsAbstract(void) const
{
	return m_Abstract;
}

/**
 * Retrieves the debug information for the configuration item.
 *
 * @returns The debug information.
 */
DebugInfo ConfigItem::GetDebugInfo(void) const
{
	return m_DebugInfo;
}

Dictionary::Ptr ConfigItem::GetScope(void) const
{
	return m_Scope;
}

ConfigObject::Ptr ConfigItem::GetObject(void) const
{
	return m_Object;
}

/**
 * Retrieves the expression list for the configuration item.
 *
 * @returns The expression list.
 */
boost::shared_ptr<Expression> ConfigItem::GetExpression(void) const
{
	return m_Expression;
}

/**
* Retrieves the object filter for the configuration item.
*
* @returns The filter expression.
*/
boost::shared_ptr<Expression> ConfigItem::GetFilter(void) const
{
	return m_Filter;
}

class DefaultValidationUtils : public ValidationUtils
{
public:
	virtual bool ValidateName(const String& type, const String& name) const override
	{
		return ConfigItem::GetByTypeAndName(type, name) != ConfigItem::Ptr();
	}
};

/**
 * Commits the configuration item by creating a ConfigObject
 * object.
 *
 * @returns The ConfigObject that was created/updated.
 */
ConfigObject::Ptr ConfigItem::Commit(bool discard)
{
	ASSERT(!OwnsLock());

#ifdef I2_DEBUG
	Log(LogDebug, "ConfigItem")
	    << "Commit called for ConfigItem Type=" << GetType() << ", Name=" << GetName();
#endif /* I2_DEBUG */

	/* Make sure the type is valid. */
	Type::Ptr type = Type::GetByName(GetType());
	ASSERT(type && ConfigObject::TypeInstance->IsAssignableFrom(type));

	if (IsAbstract())
		return ConfigObject::Ptr();

	ConfigObject::Ptr dobj = static_pointer_cast<ConfigObject>(type->Instantiate());

	dobj->SetDebugInfo(m_DebugInfo);
	dobj->SetTypeNameV(m_Type);
	dobj->SetZoneName(m_Zone);
	dobj->SetPackage(m_Package);
	dobj->SetName(m_Name);

	DebugHint debugHints;

	ScriptFrame frame(dobj);
	if (m_Scope)
		m_Scope->CopyTo(frame.Locals);
	m_Expression->Evaluate(frame, &debugHints);

	if (discard)
		m_Expression.reset();

	String item_name;
	String short_name = dobj->GetShortName();

	if (!short_name.IsEmpty()) {
		item_name = short_name;
		dobj->SetName(short_name);
	} else
		item_name = m_Name;

	String name = item_name;

	NameComposer *nc = dynamic_cast<NameComposer *>(type.get());

	if (nc) {
		name = nc->MakeName(name, dobj);

		if (name.IsEmpty())
			BOOST_THROW_EXCEPTION(std::runtime_error("Could not determine name for object"));
	}

	if (name != item_name)
		dobj->SetShortName(item_name);

	dobj->SetName(name);
	dobj->OnConfigLoaded();

	{
		boost::mutex::scoped_lock lock(m_Mutex);
		m_CommittedItems.push_back(this);
	}

	Dictionary::Ptr persistentItem = new Dictionary();

	persistentItem->Set("type", GetType());
	persistentItem->Set("name", GetName());
	persistentItem->Set("properties", Serialize(dobj, FAConfig));

	Dictionary::Ptr dhint = debugHints.ToDictionary();
	persistentItem->Set("debug_hints", dhint);

	Array::Ptr di = new Array();
	di->Add(m_DebugInfo.Path);
	di->Add(m_DebugInfo.FirstLine);
	di->Add(m_DebugInfo.FirstColumn);
	di->Add(m_DebugInfo.LastLine);
	di->Add(m_DebugInfo.LastColumn);
	persistentItem->Set("debug_info", di);

	ConfigCompilerContext::GetInstance()->WriteObject(persistentItem);
	persistentItem.reset();

	try {
		DefaultValidationUtils utils;
		dobj->Validate(FAConfig, utils);
	} catch (ValidationError& ex) {
		ex.SetDebugHint(dhint);
		throw;
	}

	dhint.reset();

	dobj->Register();

	m_Object = dobj;

	return dobj;
}

/**
 * Registers the configuration item.
 */
void ConfigItem::Register(void)
{
	Type::Ptr type = Type::GetByName(m_Type);

	/* If this is a non-abstract object with a composite name
	 * we register it in m_UnnamedItems instead of m_Items. */
	if (!m_Abstract && dynamic_cast<NameComposer *>(type.get())) {
		boost::mutex::scoped_lock lock(m_Mutex);
		m_UnnamedItems.push_back(this);
	} else {
		boost::mutex::scoped_lock lock(m_Mutex);

		ItemMap::const_iterator it = m_Items[m_Type].find(m_Name);

		if (it != m_Items[m_Type].end()) {
			std::ostringstream msgbuf;
			msgbuf << "A configuration item of type '" << GetType()
			       << "' and name '" << GetName() << "' already exists ("
			       << it->second->GetDebugInfo() << "), new declaration: " << GetDebugInfo();
			BOOST_THROW_EXCEPTION(ScriptError(msgbuf.str()));
		}

		m_Items[m_Type][m_Name] = this;
	}
}

/**
 * Unregisters the configuration item.
 */
void ConfigItem::Unregister(void)
{
	if (m_Object)
		m_Object->Unregister();

	boost::mutex::scoped_lock lock(m_Mutex);
	m_UnnamedItems.erase(std::remove(m_UnnamedItems.begin(), m_UnnamedItems.end(), this), m_UnnamedItems.end());
	m_Items[m_Type].erase(m_Name);
	m_CommittedItems.erase(std::remove(m_CommittedItems.begin(), m_CommittedItems.end(), this), m_CommittedItems.end());
}

/**
 * Retrieves a configuration item by type and name.
 *
 * @param type The type of the ConfigItem that is to be looked up.
 * @param name The name of the ConfigItem that is to be looked up.
 * @returns The configuration item.
 */
ConfigItem::Ptr ConfigItem::GetByTypeAndName(const String& type, const String& name)
{
	boost::mutex::scoped_lock lock(m_Mutex);

	ConfigItem::TypeMap::const_iterator it = m_Items.find(type);

	if (it == m_Items.end())
		return ConfigItem::Ptr();

	ConfigItem::ItemMap::const_iterator it2 = it->second.find(name);

	if (it2 == it->second.end())
		return ConfigItem::Ptr();

	return it2->second;
}

bool ConfigItem::CommitNewItems(WorkQueue& upq, std::vector<ConfigItem::Ptr>& newItems)
{
	typedef std::pair<ConfigItem::Ptr, bool> ItemPair;
	std::vector<ItemPair> items;

	{
		boost::mutex::scoped_lock lock(m_Mutex);

		BOOST_FOREACH(const TypeMap::value_type& kv, m_Items) {
			BOOST_FOREACH(const ItemMap::value_type& kv2, kv.second) {
				if (!kv2.second->m_Abstract && !kv2.second->m_Object)
					items.push_back(std::make_pair(kv2.second, false));
			}
		}

		BOOST_FOREACH(const ConfigItem::Ptr& item, m_UnnamedItems) {
			if (!item->m_Abstract && !item->m_Object)
				items.push_back(std::make_pair(item, true));
		}

		m_UnnamedItems.clear();
	}

	if (items.empty())
		return true;

	BOOST_FOREACH(const ItemPair& ip, items) {
		newItems.push_back(ip.first);
		upq.Enqueue(boost::bind(&ConfigItem::Commit, ip.first, ip.second));
	}

	upq.Join();

	if (upq.HasExceptions())
		return false;

	std::vector<ConfigItem::Ptr> new_items;

	{
		boost::mutex::scoped_lock lock(m_Mutex);
		new_items.swap(m_CommittedItems);
	}

	std::set<String> types;

	std::vector<Type::Ptr> all_types;
	Dictionary::Ptr globals = ScriptGlobal::GetGlobals();

	{
		ObjectLock olock(globals);
		BOOST_FOREACH(const Dictionary::Pair& kv, globals) {
			if (kv.second.IsObjectType<Type>())
				all_types.push_back(kv.second);
		}
	}

	BOOST_FOREACH(const Type::Ptr& type, all_types) {
		if (ConfigObject::TypeInstance->IsAssignableFrom(type))
			types.insert(type->GetName());
	}

	std::set<String> completed_types;

	while (types.size() != completed_types.size()) {
		BOOST_FOREACH(const String& type, types) {
			if (completed_types.find(type) != completed_types.end())
				continue;

			Type::Ptr ptype = Type::GetByName(type);
			bool unresolved_dep = false;

			/* skip this type (for now) if there are unresolved load dependencies */
			BOOST_FOREACH(const String& loadDep, ptype->GetLoadDependencies()) {
				if (types.find(loadDep) != types.end() && completed_types.find(loadDep) == completed_types.end()) {
					unresolved_dep = true;
					break;
				}
			}

			if (unresolved_dep)
				continue;

			BOOST_FOREACH(const ConfigItem::Ptr& item, new_items) {
				ASSERT(item->m_Object);

				if (item->m_Type == type)
					upq.Enqueue(boost::bind(&ConfigObject::OnAllConfigLoaded, item->m_Object));
			}

			completed_types.insert(type);

			upq.Join();

			if (upq.HasExceptions())
				return false;

			BOOST_FOREACH(const String& loadDep, ptype->GetLoadDependencies()) {
				BOOST_FOREACH(const ConfigItem::Ptr& item, new_items) {
					ASSERT(item->m_Object);

					if (item->m_Type == loadDep)
						upq.Enqueue(boost::bind(&ConfigObject::CreateChildObjects, item->m_Object, ptype));
				}
			}

			upq.Join();

			if (upq.HasExceptions())
				return false;

			if (!CommitNewItems(upq, newItems))
				return false;
		}
	}

	return true;
}

bool ConfigItem::CommitItems(WorkQueue& upq)
{
	Log(LogInformation, "ConfigItem", "Committing config items");

	std::vector<ConfigItem::Ptr> newItems;

	if (!CommitNewItems(upq, newItems)) {
		upq.ReportExceptions("config");

		BOOST_FOREACH(const ConfigItem::Ptr& item, newItems) {
			item->Unregister();
		}

		return false;
	}

	ApplyRule::CheckMatches();

	/* log stats for external parsers */
	typedef std::map<Type::Ptr, int> ItemCountMap;
	ItemCountMap itemCounts;
	BOOST_FOREACH(const ConfigItem::Ptr& item, newItems) {
		itemCounts[item->m_Object->GetReflectionType()]++;
	}

	BOOST_FOREACH(const ItemCountMap::value_type& kv, itemCounts) {
		Log(LogInformation, "ConfigItem")
		    << "Instantiated " << kv.second << " " << (kv.second != 1 ? kv.first->GetPluralName() : kv.first->GetName()) << ".";
	}

	return true;
}

bool ConfigItem::ActivateItems(WorkQueue& upq, bool restoreState)
{
	if (restoreState) {
		/* restore the previous program state */
		try {
			ConfigObject::RestoreObjects(Application::GetStatePath());
		} catch (const std::exception& ex) {
			Log(LogCritical, "ConfigItem")
			    << "Failed to restore state file: " << DiagnosticInformation(ex);
		}
	}

	Log(LogInformation, "ConfigItem", "Triggering Start signal for config items");

	BOOST_FOREACH(const ConfigType::Ptr& type, ConfigType::GetTypes()) {
		BOOST_FOREACH(const ConfigObject::Ptr& object, type->GetObjects()) {
			if (object->IsActive())
				continue;

#ifdef I2_DEBUG
			Log(LogDebug, "ConfigItem")
			    << "Activating object '" << object->GetName() << "' of type '" << object->GetType()->GetName() << "'";
#endif /* I2_DEBUG */
			upq.Enqueue(boost::bind(&ConfigObject::Activate, object));
		}
	}

	upq.Join();

	if (upq.HasExceptions()) {
		upq.ReportExceptions("ConfigItem");
		return false;
	}

#ifdef I2_DEBUG
	BOOST_FOREACH(const ConfigType::Ptr& type, ConfigType::GetTypes()) {
		BOOST_FOREACH(const ConfigObject::Ptr& object, type->GetObjects()) {
			ASSERT(object->IsActive());
		}
	}
#endif /* I2_DEBUG */

	Log(LogInformation, "ConfigItem", "Activated all objects.");

	return true;
}

bool ConfigItem::CommitAndActivate(void)
{
	WorkQueue upq(25000, Application::GetConcurrency());

	if (!CommitItems(upq))
		return false;

	if (!ActivateItems(upq, false))
		return false;

	return true;
}

std::vector<ConfigItem::Ptr> ConfigItem::GetItems(const String& type)
{
	std::vector<ConfigItem::Ptr> items;

	boost::mutex::scoped_lock lock(m_Mutex);

	TypeMap::const_iterator it = m_Items.find(type);

	if (it == m_Items.end())
		return items;

	BOOST_FOREACH(const ItemMap::value_type& kv, it->second)
	{
		items.push_back(kv.second);
	}

	return items;
}
