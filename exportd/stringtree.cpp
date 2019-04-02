#include "stringtree.hpp"
#include "selector_manager.hpp"
#include "crfio.hpp"
#include <core123/complaints.hpp>
#include <core123/throwutils.hpp>
#include <string>
#include <memory>
#include <map>
#include <sys/stat.h>

using namespace core123;

namespace {

// First, a helper function that splits a string on the first slash.
auto
splitfirstslash(const std::string s){
    std::pair<std::string, std::string> ret;
    auto firstslash = s.find('/');
    ret.first = s.substr(0, firstslash);
    if(firstslash != std::string::npos)
        ret.second = s.substr(firstslash+1);
    return ret;
}

// Each node of our "tree" of prefixes is a map from a path-element (e.g., "gcc"
// "4.7.2-27B") to a pointer to another node.  A node is terminal (and hence indicates
// that all names at or below it are long-timeout) if and only if the pointer is NULL.
// The code is pleasingly small, but may be "too clever by half".  In particular, it
// doesn't give us any place to hang any additional information, e.g., a timeout
// that came from the db rather than a global static.

// create a new prefix_tree with one entry at each level, corresponding
// to the elements of relpath
std::unique_ptr<stringtree> make_new_prefix_tree(const std::string& relpath){
    if(relpath.empty())
        return nullptr;
    auto firstrest = splitfirstslash(relpath);
    auto ret = std::make_unique<stringtree>();
    ret->insert( std::make_pair(firstrest.first, make_new_prefix_tree(firstrest.second)) );
    return ret;
}
} // namespace <anon>

// return the relationship between relpath and the tree at ltn.
stringtree::search_result_e search(const std::string& relpath, const stringtree* ltn){
    if(ltn == nullptr)
        return (relpath.empty()) ?  stringtree::PATH_TERMINATES_TREE : stringtree::TREE_PREFIXES_PATH;
    if(relpath.empty())
        return stringtree::PATH_IN_TREE;
    auto firstrest = splitfirstslash(relpath);
    auto fi = ltn->find(firstrest.first);
    if( fi == ltn->end() )
        return stringtree::PATH_NOT_IN_TREE;
    return search(firstrest.second, fi->second.get());
}

// Add relpath to an existing tree.
void add_prefix(const std::string& relpath, stringtree* ltn) {
    if(!ltn || relpath.empty())
        throw se(EINVAL, "nested longtimeouts?");
    auto firstrest = splitfirstslash(relpath);
    auto fi = ltn->find(firstrest.first);
    if(fi == ltn->end())
        ltn->insert( std::make_pair( firstrest.first, make_new_prefix_tree(firstrest.second)) );
    else
        add_prefix(firstrest.second, fi->second.get());
 }
