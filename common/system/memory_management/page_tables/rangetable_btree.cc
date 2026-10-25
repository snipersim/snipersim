#include <vector>
#include <utility>
#include "core.h"
#include "fixed_types.h"
#include "rangetable.h"
#include "rangetable_btree.h"
#include <tuple>
#include "simulator.h"
#include "mimicos.h"
#include <tuple>
//#define DEBUG

using namespace std;

namespace ParametricDramDirectoryMSI
{
    RangeTableBtree::RangeTableBtree(String name, int temp, int app_id) : RangeTable(name, app_id)
    {

        log_file_name = "range_table_btree.log";
        log_file_name = std::string(Sim()->getConfig()->getOutputDirectory().c_str()) + "/" + log_file_name;
        log_file.open(log_file_name);
        root = NULL;
        t = temp;

    }

    TreeNode::TreeNode(int t1, bool leaf1)
    {
        t = t1;
        leaf = leaf1;

        keys = new std::pair<uint64_t, uint64_t>[2 * t - 1];
        values = new RangeEntry[2 * t - 1];
        C = new TreeNode *[2 * t];
        emulated_ppn = Sim()->getMimicOS()->getMemoryAllocator()->handle_page_table_allocations(4096);

        n = 0;
    }

    void TreeNode::traverse()
    {
        int i;
        for (i = 0; i < n; i++)
        {
            if (leaf == false)
                C[i]->traverse();
        }

        if (leaf == false)
            C[i]->traverse();
    }

    std::pair<TreeNode*,int> TreeNode::search(IntPtr k, std::vector<IntPtr> &accessedAddresses)
    {
        int i = 0;
        while (i < n && k > keys[i].second)
        {
            accessedAddresses.push_back(emulated_ppn*4096 + i * 24);
            i++;
        }

        if (keys[i].first <= k && keys[i].second > k)
            return std::make_pair(this, i);

        if (leaf == true)
            return std::make_pair(static_cast<ParametricDramDirectoryMSI::TreeNode*>(NULL), 0);

        return C[i]->search(k, accessedAddresses);
    }

    void RangeTableBtree::insert(std::pair<uint64_t, uint64_t> k, RangeEntry value)
    {
        if (root == NULL)
        {
            root = new TreeNode(t, true);
            root->keys[0] = k;
            root->values[0] = value;   // Store RangeEntry for the first insertion
            root->n = 1;
#ifdef DEBUG
            log_file << "Inserting key: " << k.first << " - " << k.second << " with value: " << value.offset << std::endl;
#endif
        }
        else
        {
#ifdef DEBUG
            log_file << "Inserting key: " << k.first << " - " << k.second << " with value: " << value.offset << std::endl;
#endif
            if (root->n == 2 * t - 1)
            {
                TreeNode *s = new TreeNode(t, false);

                s->C[0] = root;

                s->splitChild(0, root);

                int i = 0;
                if (s->keys[0] < k)
                    i++;
                s->C[i]->insertNonFull(k, value);

                root = s;
            }
            else
                root->insertNonFull(k, value);
        }
    }

    void TreeNode::insertNonFull(std::pair<uint64_t, uint64_t> k, RangeEntry value)
    {
        int i = n - 1;

        if (leaf == true)
        {
            while (i >= 0 && keys[i] > k)
            {
                keys[i + 1] = keys[i];
                values[i + 1] = values[i];
                i--;
            }

            keys[i + 1] = k;
            values[i + 1] = value;
            n = n + 1;
        }
        else
        {
            while (i >= 0 && keys[i] > k)
                i--;

            if (C[i + 1]->n == 2 * t - 1)
            {
                splitChild(i + 1, C[i + 1]);

                if (keys[i + 1] < k)
                    i++;
            }
            C[i + 1]->insertNonFull(k, value);
        }
    }

    void TreeNode::splitChild(int i, TreeNode *y)
    {
        TreeNode *z = new TreeNode(y->t, y->leaf);
        z->n = t - 1;

        for (int j = 0; j < t - 1; j++){
            z->keys[j] = y->keys[j + t];
            z->values[j] = y->values[j + t];
        }

        if (y->leaf == false)
        {
            for (int j = 0; j < t; j++)
                z->C[j] = y->C[j + t];
        }

        y->n = t - 1;
        for (int j = n; j >= i + 1; j--)
            C[j + 1] = C[j];

        C[i + 1] = z;

        for (int j = n - 1; j >= i; j--){
            keys[j + 1] = keys[j];
            values[j + 1] = values[j];
        }

        keys[i] = y->keys[t - 1];
        values[i] = y->values[t - 1];
        
        n = n + 1;
    }

}
