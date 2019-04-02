#pragma once

#include <string>
#include <map>
#include <memory>

// A tree of strings, more-or-less like a directory hierarchy.
//
// Create an empty tree with a default constructor.
//
// Add a path to an existing tree with addpath.
//
// Search for a path in an existing tree with search, obtaining
// one of four possible relationships.

struct stringtree : public std::map<std::string, std::unique_ptr<stringtree> > {
// There are four possible relationships between relpath and a tree:
//   PATH_IN_TREE relpath is a prefix of one or more nodes of the tree
//      e.g.,   relpath=foo/bar/baz,  tree contains a terminal foo/bar/baz/bletch
//   TREE_PREFIXES_PATH a terminal node of the tree is a prefix of relpath
//      e.g.,   relpath=foo/bar/baz,  tree contains a  terminal foo/bar
//   PATH_TERMINATES_TREE relpath exactly matches a terminal node of the tree,
//      e.g.,    relpath=foo/bar/baz  tree contains a terminal foo/bar/baz
//   PATH_NOT_IN_TREE relpath is outside the tree
//      e.g.,   relpath=foo/bar/baz,  tree contains foo/bar/bletch, foo/bleep, etc.
//
    enum search_result_e {PATH_IN_TREE, TREE_PREFIXES_PATH, PATH_TERMINATES_TREE, PATH_NOT_IN_TREE}; 
};

// Not members because we make use of the possibility the stringtree pointer arg may be nullptr.
void add_prefix(const std::string& relpath, stringtree*);
stringtree::search_result_e search(const std::string& relpath, const stringtree*);
