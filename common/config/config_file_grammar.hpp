// Config Class
// Author: Charles Gruenwald III
// #define BOOST_SPIRIT_RULE_SCANNERTYPE_LIMIT 2
#include "config_file.hpp"
// #define BOOST_SPIRIT_DEBUG        // define this for debug output

#include <boost/version.hpp>
#if (BOOST_VERSION==103500)
# include <boost/spirit/core.hpp>
# include <boost/spirit/tree/parse_tree.hpp>
# include <boost/spirit/tree/ast.hpp>
# include <boost/spirit/utility/confix.hpp>
# include <boost/spirit/utility/escape_char.hpp>
# include <boost/spirit/utility/chset.hpp>
#else
# include <boost/spirit/include/classic_core.hpp>
# include <boost/spirit/include/classic_parse_tree.hpp>
# include <boost/spirit/include/classic_ast.hpp>
# include <boost/spirit/include/classic_confix.hpp>
# include <boost/spirit/include/classic_escape_char.hpp>
# include <boost/spirit/include/classic_chset.hpp>
#endif

#include <iostream>

/*! \file config_file_grammar.hpp
 * This file represents the parser element of the bl::config::ConfigFile interface.
 * It uses boost:spirit to create an AST of the loaded config text, then simply
 * walks that tree adding the appropriate sections and keys to the ConfigFile class.
 */

namespace config
{
#if (BOOST_VERSION==103500)
    using namespace boost::spirit;
#else
    using namespace boost::spirit::classic;
#endif

    enum RuleID { defaultID, sectionID, keyNameID, keyValueID, keySeparatorID, keyValueArrayID, keyValueSpanID, keyID, sectionNameID, stringID, configID };

    // Like int_p, but support up to SInt64
    typedef int_parser<SInt64, 10, 1, -1> long_parser_t;
    static long_parser_t long_p;

    struct config_parser : public grammar<config_parser> {


        // A utility function for tagging each node with an 'ID' for identification
        // when we walk the parse tree.
        struct Name {
            Name(RuleID e_) : e(e_) { }

            template<typename Node, typename It>
                void operator()(Node& n, It, It) const {
                    n.value.value(e);
                }
            RuleID e;
        };

        // The actual node structure in the tree, we simply need an id in this structure
        // as the strings are provided by the container class provided by spirit.
        struct NodeValue {
            RuleID e;

            NodeValue() : e((RuleID)0) {}
            NodeValue(RuleID e_) : e(e_) {}
        };

        void show_match(const char *begin, const char*end)
        {
            std::cout << "Matched: " << String(begin, end-begin) << std::endl;
        }

        typedef node_iter_data_factory<NodeValue> factory_t;
        typedef const char* iterator_t;

        template <typename ScannerT>
            struct definition {
                definition(config_parser const& self) {
                    //Grammar Definition

                    // Complex rules
                    r_file         =  r_config_node  >> end_p;

                    r_config_node  =  r_config;

                    r_config       =  *r_section_node ;

                    r_section_node =  access_node_d[ r_section ][Name(sectionNameID)];

                    r_section      =  root_node_d[r_section_name_node] >> *r_key_node ;

                    r_key_node     =  access_node_d[ r_key ][Name(keyID)];

                    r_key          =  r_key_name
                                  >> lexeme_d[ no_node_d[ *r_key_array ] >> no_node_d[ *space_p ] >> root_node_d[ ch_p('=') ] >> no_node_d[ *space_p ] ]
                                  >> !r_value ;

                    r_key_name     =  access_node_d[ !r_string ][Name(keyNameID)];

                    r_key_array    =  ch_p('[') >> ch_p(']');

                    r_value_array  =  access_node_d[ r_value_array2 ][Name(keyValueArrayID)];

                    r_value_array2 =  r_value_single_or_empty >> +(access_node_d[ ch_p(',') ][Name(keySeparatorID)] >> r_value_single_or_empty);

                    r_value_span   =  access_node_d[ r_value_long >> root_node_d[ ch_p('-') ] >> r_value_long ][Name(keyValueSpanID)];

                    r_value_long   =  access_node_d[ long_p ][Name(keyValueID)];

                    r_value_single =  access_node_d[ r_string ][Name(keyValueID)];

                    r_value_single_or_empty = r_value_single | !r_value_single;

                    r_value        =  r_value_array | r_value_span | r_value_single;

                    // Terminals
                    // Note, the lexeme directive tells the parser to act character-by-character (don't skip spaces),
                    // the token_node_d says to make 1 node out of the matched results (instead of 1 node per character)
                    // inner_node directive states to throw away each of the '[' and ']'
                    // finally config_p looks for matching [ with ]
                    r_section_name_node = access_node_d[ r_section_name ][Name(sectionNameID)] ;

                    //HACK:
                    //
                    //This is a total hack below. Because we are using Boost 1.34, we cannot use the gen_pt_node_d
                    //in certain places (notably the one commented below). The gen_pt_node_d function allows us to
                    //generate an AST node, even if there is no data contained within the node. This happens when we
                    //get a section that doesn't have a name in it (i.e. []). In Boost 1.35 we can use the following
                    //gent_pt_node_d with the inner_node_d to eliminate the braces at parse-time. Instead until then
                    //we have to leave the braces in due to null-node-culling and strip them at the time we walk the
                    //tree.

                    r_section_name = lexeme_d[ token_node_d[ confix_p('[', (*lex_escape_ch_p), ']') ] ] ;

                    // Strings and names may either be "quoted" or not.
                    r_string       =
                       (
                          ( no_node_d[ *space_p ] >> lexeme_d[ token_node_d[ inner_node_d[ confix_p('"', *lex_escape_ch_p, '"') ] ] ] >> no_node_d[ *space_p ] )
                          |
                          ( lexeme_d[ token_node_d[ +(alnum_p | (punct_p - ch_p('=') - ch_p(',') - ch_p('[') - ch_p(']') ) ) ] ] )
                          )
                       ;
                }

                rule<ScannerT> r_file, r_config, r_config_node, r_section, r_key, r_key_name, r_key_array, r_value, r_value_long, r_value_single, r_value_single_or_empty, r_value_array, r_value_array2, r_value_span, r_key_node, r_section_node, r_section_name, r_section_name_node, r_section_name_node_node, r_string;

                rule<ScannerT> const&
                    start() const { return r_file; }
            };
    };

    typedef tree_parse_info<config_parser::iterator_t, config_parser::factory_t> parse_info_t;
    typedef tree_match<config_parser::iterator_t, config_parser::factory_t> tree_match_t;
    typedef tree_match_t::node_t node_t;
    typedef tree_match_t::const_tree_iterator tree_iter_t;

} //namespaces

