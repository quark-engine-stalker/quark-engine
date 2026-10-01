#ifndef _FIXEDMAP_H
#define _FIXEDMAP_H
#pragma once

template <class K, class T, class allocator = xr_allocator>
class FixedMAP
{
	enum
	{
		SG_REALLOC_ADVANCE = 64
	};

public:
	struct TNode
	{
		K key;
		T val;
		TNode *left, *right;
	};

	typedef void __fastcall callback(TNode*);
	typedef bool __fastcall callback_cmp(TNode& N1, TNode& N2);

private:
	TNode* nodes;
	u32 pool;
	u32 limit;
	u32 last_insert_index;
	u32 previous_insert_index;

	IC TNode* remember_insert(TNode* node)
	{
		const u32 index = u32(node - nodes);
		if (index != last_insert_index)
		{
			previous_insert_index = last_insert_index;
			last_insert_index = index;
		}
		return node;
	}

	IC u32 Size(u32 Count)
	{
		return Count * sizeof(TNode);
	}

	void Realloc()
	{
		u32 newLimit = limit + SG_REALLOC_ADVANCE;
		VERIFY(newLimit%SG_REALLOC_ADVANCE == 0);
		TNode* newNodes = (TNode*)allocator::alloc(sizeof(TNode) * newLimit);
		VERIFY(newNodes);

		ZeroMemory(newNodes, Size(newLimit));
		if (limit)
			CopyMemory(newNodes, nodes, Size(limit));

		for (u32 I = 0; I < pool; I++)
		{
			VERIFY(nodes);
			TNode* Nold = nodes + I;
			TNode* Nnew = newNodes + I;

			if (Nold->left)
			{
				size_t Lid = Nold->left - nodes;
				Nnew->left = newNodes + Lid;
			}
			if (Nold->right)
			{
				size_t Rid = Nold->right - nodes;
				Nnew->right = newNodes + Rid;
			}
		}
		if (nodes) allocator::dealloc(nodes);

		nodes = newNodes;
		limit = newLimit;
	}

	IC TNode* Alloc(const K& key)
	{
		if (pool == limit) Realloc();
		TNode* node = nodes + pool;
		node->key = key;
		node->right = node->left = 0;
		pool++;
		return node;
	}

	IC TNode* CreateChild(TNode*& parent, const K& key)
	{
		size_t PID = size_t(parent - nodes);
		TNode* N = Alloc(key);
		parent = nodes + PID;
		return N;
	}

	IC void recurseLR(TNode* N, callback CB)
	{
		if (N->left) recurseLR(N->left, CB);
		CB(N);
		if (N->right) recurseLR(N->right, CB);
	}

	IC void recurseRL(TNode* N, callback CB)
	{
		if (N->right) recurseRL(N->right, CB);
		CB(N);
		if (N->left) recurseRL(N->left, CB);
	}

	template <typename container_type>
	IC void getLR(TNode* N, container_type& D)
	{
		if (N->left) getLR(N->left, D);
		D.push_back(N->val);
		if (N->right) getLR(N->right, D);
	}

	template <typename container_type>
	IC void getRL(TNode* N, container_type& D)
	{
		if (N->right) getRL(N->right, D);
		D.push_back(N->val);
		if (N->left) getRL(N->left, D);
	}

	template <typename container_type>
	IC void getLR_P(TNode* N, container_type& D)
	{
		if (N->left) getLR_P(N->left, D);
		D.push_back(N);
		if (N->right) getLR_P(N->right, D);
	}

	template <typename container_type>
	IC void getRL_P(TNode* N, container_type& D)
	{
		if (N->right) getRL_P(N->right, D);
		D.push_back(N);
		if (N->left) getRL_P(N->left, D);
	}

public:
	FixedMAP()
	{
		pool = 0;
		limit = 0;
		nodes = 0;
		last_insert_index = u32(-1);
		previous_insert_index = u32(-1);
	}

	~FixedMAP()
	{
		destroy();
	}

	void destroy()
	{
		if (nodes)
		{
			for (TNode* cur = begin(); cur != last(); cur++)
				cur->~TNode();
			allocator::dealloc(nodes);
		}
		nodes = 0;
		pool = 0;
		limit = 0;
		last_insert_index = u32(-1);
		previous_insert_index = u32(-1);
	}

	IC TNode* insert(const K& k)
	{
		// Render graph submissions commonly repeat the same shader resources.
		// Keep an index instead of a pointer so that Realloc() cannot invalidate it.
		if (last_insert_index < pool)
		{
			TNode* cached = nodes + last_insert_index;
			if (!(k < cached->key) && !(k > cached->key))
				return cached;
		}
		// Draw submissions frequently alternate between two adjacent material/state
		// groups.  A second index catches that pattern without storing pointers that
		// Realloc() could invalidate.
		if (previous_insert_index < pool)
		{
			TNode* cached = nodes + previous_insert_index;
			if (!(k < cached->key) && !(k > cached->key))
				return remember_insert(cached);
		}

		if (pool)
		{
			TNode* node = nodes;

		once_more:
			if (k < node->key)
			{
				if (node->left)
				{
					node = node->left;
					goto once_more;
				}
				else
				{
					TNode* N = CreateChild(node, k);
					node->left = N;
					return remember_insert(N);
				}
			}
			else if (k > node->key)
			{
				if (node->right)
				{
					node = node->right;
					goto once_more;
				}
				else
				{
					TNode* N = CreateChild(node, k);
					node->right = N;
					return remember_insert(N);
				}
			}
			else return remember_insert(node);
		}
		else
		{
			return remember_insert(Alloc(k));
		}
	}

	IC TNode* insertInAnyWay(const K& k)
	{
		if (pool)
		{
			TNode* node = nodes;

		once_more:
			if (k <= node->key)
			{
				if (node->left)
				{
					node = node->left;
					goto once_more;
				}
				else
				{
					TNode* N = CreateChild(node, k);
					node->left = N;
					return N;
				}
			}
			else
			{
				if (node->right)
				{
					node = node->right;
					goto once_more;
				}
				else
				{
					TNode* N = CreateChild(node, k);
					node->right = N;
					return N;
				}
			}
		}
		else
		{
			return Alloc(k);
		}
	}

	IC TNode* insert(const K& k, const T& v)
	{
		TNode* N = insert(k);
		N->val = v;
		return N;
	}

	IC TNode* insertInAnyWay(const K& k, const T& v)
	{
		TNode* N = insertInAnyWay(k);
		N->val = v;
		return N;
	}

	IC void discard()
	{
		if (nodes) allocator::dealloc(nodes);
		nodes = 0;
		pool = 0;
		limit = 0;
		last_insert_index = u32(-1);
		previous_insert_index = u32(-1);
	}

	IC u32 allocated() { return this->limit; }
	IC void clear()
	{
		pool = 0;
		last_insert_index = u32(-1);
		previous_insert_index = u32(-1);
	}
	IC TNode* begin() { return nodes; }
	IC TNode* end() { return nodes + pool; }
	IC TNode* last() { return nodes + limit; } // for setup only
	IC u32 size() { return pool; }
	IC TNode& operator[](int v) { return nodes[v]; }

	IC void traverseLR(callback CB)
	{
		if (pool) recurseLR(nodes, CB);
	}

	IC void traverseRL(callback CB)
	{
		if (pool) recurseRL(nodes, CB);
	}

	IC void traverseANY(callback CB)
	{
		TNode* _end = end();
		for (TNode* cur = begin(); cur != _end; cur++)
			CB(cur);
	}

	template <typename container_type>
	IC void getLR(container_type& D)
	{
		if (pool) getLR(nodes, D);
	}

	template <typename container_type>
	IC void getLR_P(container_type& D)
	{
		if (pool) getLR_P(nodes, D);
	}

	template <typename container_type>
	IC void getRL(container_type& D)
	{
		if (pool) getRL(nodes, D);
	}

	template <typename container_type>
	IC void getRL_P(container_type& D)
	{
		if (pool) getRL_P(nodes, D);
	}

	template <typename container_type>
	IC void getANY(container_type& D)
	{
		TNode* _end = end();
		for (TNode* cur = begin(); cur != _end; cur++) D.push_back(cur->val);
	}

	template <typename container_type>
	IC void getANY_P(container_type& D)
	{
		if (!pool)
		{
			D.clear();
			return;
		}

		D.resize(size());
		TNode** _it = D.data();
		TNode* _end = end();
		for (TNode* cur = begin(); cur != _end; cur++, _it++) *_it = cur;
	}

	IC void getANY_P(xr_vector<void*, typename allocator::template helper<void*>::result>& D)
	{
		if (!pool)
		{
			D.clear();
			return;
		}

		D.resize(size());
		void** _it = D.data();
		TNode* _end = end();
		for (TNode* cur = begin(); cur != _end; cur++, _it++) *_it = cur;
	}

	IC void setup(callback CB)
	{
		for (int i = 0; i < limit; i++)
			CB(nodes + i);
	}
};
#endif
