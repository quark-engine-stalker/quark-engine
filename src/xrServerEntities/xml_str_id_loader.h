#pragma once

#ifdef XRGAME_EXPORTS
#	include "ui/xrUIXmlParser.h"
#else // XRGAME_EXPORTS
#	include "xrUIXmlParser.h"
#	include "object_broker.h"
#endif // XRGAME_EXPORTS


//T_ID    - уникальный текстовый идентификатор (аттрибут id в XML файле)
//T_INDEX - уникальный числовой индекс 
//T_INIT -  класс где определена статическая InitXmlIdToIndex
//          функция инициализации file_str и tag_name

//структура хранит строковый id элемента 
//файл и позицию, где этот элемент находится
struct ITEM_DATA
{
	shared_str id;
	int index;
	int pos_in_file;
	CUIXml* _xml;
	XML_NODE* _node;
};

typedef xr_vector<ITEM_DATA> T_VECTOR;
typedef xr_unordered_flat_map<shared_str, u32> T_ID_LOOKUP;

void _destroy_item_data_vector_cont(T_VECTOR* vec);

#define TEMPLATE_SPECIALIZATION template<typename T_INIT>
#define CSXML_IdToIndex CXML_IdToIndex<T_INIT>

TEMPLATE_SPECIALIZATION
class CXML_IdToIndex
{
public:

private:
	static T_VECTOR* m_pItemDataVector;
	static T_ID_LOOKUP* m_pItemIdLookup;

protected:
	//имена xml файлов (разделенных запятой) из которых 
	//производить загрузку элементов
	static LPCSTR file_str;
	//имена тегов
	static LPCSTR tag_name;
public:
	CXML_IdToIndex();
	virtual ~CXML_IdToIndex();

	static void InitInternal();

	static const ITEM_DATA* GetById(const shared_str& str_id, bool no_assert = false);
	static const ITEM_DATA* GetByIndex(int index, bool no_assert = false);

	static int IdToIndex(const shared_str& str_id, int default_index = -1, bool no_assert = false)
	{
		const ITEM_DATA* item = GetById(str_id, no_assert);
		return item ? item->index : default_index;
	}

	static shared_str IndexToId(int index, shared_str default_id = shared_str(), bool no_assert = false)
	{
		const ITEM_DATA* item = GetByIndex(index, no_assert);
		return item ? item->id : default_id;
	}

	static int GetMaxIndex() { return static_cast<int>(m_pItemDataVector->size()) - 1; }

	//удаление статичекого массива
	static void DeleteIdToIndexData();
};


TEMPLATE_SPECIALIZATION
T_VECTOR* CSXML_IdToIndex::m_pItemDataVector = NULL;

TEMPLATE_SPECIALIZATION
T_ID_LOOKUP* CSXML_IdToIndex::m_pItemIdLookup = NULL;

TEMPLATE_SPECIALIZATION
LPCSTR CSXML_IdToIndex::file_str = NULL;
TEMPLATE_SPECIALIZATION
LPCSTR CSXML_IdToIndex::tag_name = NULL;


TEMPLATE_SPECIALIZATION
CSXML_IdToIndex::CXML_IdToIndex()
{
}


TEMPLATE_SPECIALIZATION
CSXML_IdToIndex::~CXML_IdToIndex()
{
}


TEMPLATE_SPECIALIZATION
const ITEM_DATA* CSXML_IdToIndex::GetById(const shared_str& str_id, bool no_assert)
{
	T_INIT::InitXmlIdToIndex();
	const T_ID_LOOKUP::const_iterator lookup_it = m_pItemIdLookup->find(str_id);

	if (m_pItemIdLookup->end() == lookup_it)
	{
		int i = 0;
		Msg("item with id [%s] not found in files:", str_id.c_str());
		for (T_VECTOR::const_iterator it = m_pItemDataVector->begin(); m_pItemDataVector->end() != it; ++it, ++i)
			Msg("[%d]=[%s]", i, *(*it).id);

		R_ASSERT3(no_assert, "item not found, id", str_id.c_str());
		return NULL;
	}

	return &(*m_pItemDataVector)[lookup_it->second];
}

TEMPLATE_SPECIALIZATION
const ITEM_DATA* CSXML_IdToIndex::GetByIndex(int index, bool no_assert)
{
	if ((size_t)index >= m_pItemDataVector->size())
	{
		R_ASSERT3(no_assert, "item by index not found in files", file_str);
		return NULL;
	}
	return &(*m_pItemDataVector)[index];
}

TEMPLATE_SPECIALIZATION
void CSXML_IdToIndex::DeleteIdToIndexData()
{
	VERIFY(m_pItemDataVector);
	VERIFY(m_pItemIdLookup);
	_destroy_item_data_vector_cont(m_pItemDataVector);

	xr_delete(m_pItemIdLookup);
	xr_delete(m_pItemDataVector);
}

TEMPLATE_SPECIALIZATION
void CSXML_IdToIndex::InitInternal()
{
	VERIFY(!m_pItemDataVector);
	VERIFY(!m_pItemIdLookup);
	T_INIT::InitXmlIdToIndex();

	m_pItemDataVector = xr_new<T_VECTOR>();
	m_pItemIdLookup = xr_new<T_ID_LOOKUP>();

	VERIFY(file_str);
	VERIFY(tag_name);

	string_path xml_file;
	int count = _GetItemCount(file_str);
	int index = 0;
	for (int it = 0; it < count; ++it)
	{
		_GetItem(file_str, it, xml_file);

		CUIXml* uiXml = xr_new<CUIXml>();
		xr_string xml_file_full;
		xml_file_full = xml_file;
		xml_file_full += ".xml";
		uiXml->Load(CONFIG_PATH, "gameplay", xml_file_full.c_str());

		const int items_num = uiXml->GetNodesNum(uiXml->GetRoot(), tag_name);
		const size_t required_size = m_pItemDataVector->size() + static_cast<size_t>(items_num);
		m_pItemDataVector->reserve(required_size);
		m_pItemIdLookup->reserve(required_size);

		XML_NODE* item_node = items_num ? uiXml->GetRoot()->FirstChild(tag_name) : NULL;
		for (int i = 0; i < items_num; ++i)
		{
			R_ASSERT3(item_node, "item node not found in file", xml_file);
			LPCSTR item_name = uiXml->ReadAttrib(item_node, "id", NULL);

			string256 buf;
			xr_sprintf(buf, "id for item don't set, number %d in %s", i, xml_file);
			R_ASSERT2(item_name, buf);

			const shared_str item_id(item_name);
			const bool inserted = m_pItemIdLookup->emplace(item_id, static_cast<u32>(index)).second;
			R_ASSERT3(inserted, "duplicate item id", item_name);

			ITEM_DATA data;
			data.id = item_id;
			data.index = index;
			data.pos_in_file = i;
			data._xml = uiXml;
			data._node = item_node;
			m_pItemDataVector->push_back(data);

			++index;
			item_node = uiXml->GetRoot()->IterateChildren(tag_name, item_node);
		}
		if (0 == items_num)
			delete_data(uiXml);
	}
}

#undef TEMPLATE_SPECIALIZATION
