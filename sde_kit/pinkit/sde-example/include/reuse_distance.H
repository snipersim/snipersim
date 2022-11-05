// 
// Copyright (C) 2014-2019 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

#ifndef REUSE_DISTANCE_H
#define REUSE_DISTANCE_H

#include <set>
#include <map>

static UINT32 lzcount(UINT64 v) {
    // From Hacker's Delight. modified and extended to 64b.
    // License: http://hackersdelight.org/permissions.htm
    // (free to use, no attribution required)

    UINT32 n=64;
    UINT64 y,x=v;
    if (v == 0) return 64;
    y = x >> 32; if (y!=0) { n -= 32; x=y; }
    y = x >> 16; if (y!=0) { n -= 16; x=y; }
    y = x >> 8;  if (y!=0) { n -= 8;  x=y; }
    y = x >> 4;  if (y!=0) { n -= 4;  x=y; }
    y = x >> 2;  if (y!=0) { n -= 2;  x=y; }
    y = x >> 1;  if (y!=0) { return n-2; }
    return n-x;
}

static UINT32 int_log2(UINT64 v)
{
    return sizeof(UINT64)*8 - 1 - lzcount(v) ;
}

class RD
{
    public:
        virtual ~RD() {}
        virtual UINT32 reference(ADDRINT address) = 0;
};

class RD_Treap : public RD
{
  private:
    // Each Node is an address
    // Nodes are kept in a tree, where pre-order traversal visits them 
    // in stack distance
    // I.e. the root node is first on the stack, then its left subtree, then 
    // its right subtree
    // Each node has a count field which counts the number of nodes below it 
    // (including self)
    struct Node
    {
        const ADDRINT _address;
        Node *_parent, *_left, *_right;
        UINT64 _count;

        Node(ADDRINT address) : _address(address), _parent(NULL),
            _left(NULL), _right(NULL), _count(1) {}
        bool isRightChild() const { return _parent && _parent->_right == this;}
        void setLeft(Node *left)
        {
            _left = left;
            if (left) left->_parent = this;
        }
        void setRight(Node *right)
        {
            _right = right;
            if (right) right->_parent = this;
        }
        void updateCount(bool recurse)
        {
            _count = (_left ? _left->_count : 0) 
                + 1 + (_right ? _right->_count : 0);
            if (recurse && _parent)
                _parent->updateCount(recurse);
        }
        void ASSERTXCount()
        {
            ASSERTX(_count == (_left ? _left->_count : 0) + 1
                + (_right ? _right->_count : 0));
            if (_left) _left->ASSERTXCount();
            if (_right) _right->ASSERTXCount();
        }
        void stealChild(Node *fromNode)
        {
            if (fromNode->_right)
            {
                setRight(fromNode->_right);
                fromNode->setRight(NULL);
                if (fromNode->_left)
                    fromNode->stealChild(fromNode->_left);
            }
            else
            {
                setRight(fromNode->_left);
                fromNode->setLeft(NULL);
            }
            fromNode->updateCount(false);
            this->updateCount(false);
        }
        void insertAtRoot(Node *currentRoot)
        {
            _parent = NULL;
            setLeft(currentRoot);
            // Steal one of currentRoot's children as our right child
            stealChild(currentRoot);
        }
        void moveToRoot(Node *currentRoot)
        {
            while(_parent)
            {
                // At most one child: remove from where we are now,
                // move child in our place, and set as root immediately
                if (!_left || !_right)
                {
                    // Remove from old position
                    Node *child = _left ? _left : _right;
                    if (_parent->_left == this) _parent->setLeft(child);
                    else                        _parent->setRight(child);
                    _parent->updateCount(true);
                    this->insertAtRoot(currentRoot);
                }
                else
                {
                    // Swap with parent to move node up
                    Node *pparent = _parent->_parent, *parent = _parent,
                        *left = _left, *right = _right;
                    if (parent->_left == this)
                    {
                        this->setLeft(parent);
                        this->setRight(parent->_right);
                        parent->setLeft(left);
                        parent->setRight(right);
                    }
                    else
                    {
                        this->setLeft(parent);
                        parent->setRight(left);
                    }
                    if (!pparent)
                        _parent = NULL;
                    else if (pparent->_left == parent)
                        pparent->setLeft(this);
                    else
                        pparent->setRight(this);
                    parent->updateCount(false);
                    this->updateCount(false);
                }
            }
        }
        UINT64 sumLeft() const
        {
            // Return number of nodes preceding its right child
            return computePosition() + (_left ? _left->_count : 0);
        }
        UINT64 computePosition() const
        {
            if (isRightChild())
                return _parent->sumLeft() + 1;
            else if (_parent)
                return _parent->computePosition() + 1;
            else
                return 1;
        }
        void print()
        {
            std::cout << " " << _address << "[" <<  _count << "] ";
            std::cout << "("; if (_left) _left->print(); std::cout << ")";
            std::cout << "("; if (_right) _right->print(); std::cout << ")";
        }
        
    };
    std::map<ADDRINT, Node*> _map;
    Node *_root;
  public:
    RD_Treap() : _root(NULL) {}

    ~RD_Treap() 
    {
        for(std::map<ADDRINT, Node*>::iterator it = _map.begin(); 
            it != _map.end(); ++it)
        {
            delete it->second;
        }
    }

    UINT32 reference(ADDRINT address)
    {
        Node *node;
        UINT64 dist;
        if (_map.count(address))
        {
            node = _map[address];
            dist = node->computePosition();
            node->moveToRoot(_root);
        }
        else
        {
            node = new Node(address);
            _map[address] = node;
            dist = INT_MAX;
            if (_root)
                node->insertAtRoot(_root);
        }
        _root = node;

        return int_log2(dist);
    }
};

class RD_LogRR : public RD
{
    private:
        /*
         * Keep addresses in a set of pools, each one 2x larger than the 
         * previous one.
         * Pools are stored back-to-back in a single list `entries_list'.
         * Addresses are replaced round-robin in each pool.
         * Address positions are also stored in `entries_map', keyed by address.
         * On each access, the address' position is looked up, and it is moved
         * to the first pool -- pushing one address from each pool into the 
         * next pool, until (a) the entry left free by the original address is 
         * replaced  or (b) an address falls off the end 
         * (if the original access was a miss).
         *
         * This resembles a multi-level exclusive cache hierarchy, the position
         * at which an address is found should be close to its reuse distance
         * (it would be exact if the pools used LRU replacement).
        */
        static const UINT32 MIN_SIZE_BITS = 10;
        static const UINT32 MAX_SIZE_BITS = 24;
        static const UINT64 MIN_SIZE = UINT64(1) << MIN_SIZE_BITS;
        static const UINT64 MAX_SIZE = UINT64(1) << MAX_SIZE_BITS;
        std::map<ADDRINT, UINT64> entries_map;
        std::vector<ADDRINT> entries_list;
        UINT64 replace_position[MAX_SIZE_BITS];
        UINT64 getIdx(UINT32 bin, UINT64 idx)
        {
            if (bin == MIN_SIZE_BITS)
                return idx;
            else
                return (UINT64(1) << (bin - 1)) + idx;
        }
    public:
        RD_LogRR() : entries_list(0)
        {
            memset(replace_position,0,sizeof(replace_position));
        }
        UINT32 reference(ADDRINT address)
        {
            // Lazy allocation so we only consume memory for threads 
            // that are actually used
            // rather than all PIN_MAX_THREADS of them
            if (entries_list.size() == 0)
                entries_list.resize(MAX_SIZE);

            UINT64 position;
            UINT32 pos_log2;
            if (entries_map.count(address))
            {
                position = entries_map[address];
                if (position < MIN_SIZE)
                    return MIN_SIZE_BITS;
                pos_log2 = int_log2(position);
            }
            else
            {
                position = MAX_SIZE;
                pos_log2 = MAX_SIZE_BITS;
            }

            ADDRINT item = address;
            for(UINT32 bin = MIN_SIZE_BITS; bin < pos_log2; ++bin)
            {
                UINT64 idx = getIdx(bin, replace_position[bin]);
                replace_position[bin] = (replace_position[bin] + 1) 
                    & ((UINT64(1) << (bin - 1)) - 1);
                ADDRINT replace_item = entries_list[idx];
                entries_list[idx] = item;
                entries_map[item] = idx;
                item = replace_item;
                if (item == 0)
                    break;
            }
            if (pos_log2 < MAX_SIZE_BITS)
            {
                entries_list[position] = item;
                entries_map[item] = position;
            }
            else if (item)
            {
                entries_map.erase(item);
            }

            return pos_log2;
        }
};

#endif // REUSE_DISTANCE_H
