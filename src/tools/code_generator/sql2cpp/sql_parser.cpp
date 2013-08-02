/*
	Copyright (C) 2013 by Pierre Talbot <ptalbot@mopong.net>
	Part of the Battle for Wesnoth Project http://www.wesnoth.org/

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY.

	See the COPYING file for more details.
*/

#include <boost/spirit/include/qi.hpp>
#include <boost/spirit/include/lex_lexertl.hpp>
#include <boost/spirit/include/phoenix.hpp>

#include <boost/fusion/include/adapt_struct.hpp>

#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>
#include <boost/static_assert.hpp>

#include "tools/code_generator/sql2cpp/sql_type.hpp"
#include "tools/code_generator/sql2cpp/sql_type_constraint.hpp"

#include <iostream>
#include <fstream>
#include <string>

namespace bs = boost::spirit;
namespace lex = boost::spirit::lex;
namespace qi = boost::spirit::qi;
namespace phx = boost::phoenix;

// Token definition base, defines all tokens for the base grammar below
template <typename Lexer>
struct sql_tokens : lex::lexer<Lexer>
{
public:
	// Tokens with no attributes.
	lex::token_def<lex::omit> type_smallint, type_int, type_varchar, type_text, type_date;
	lex::token_def<lex::omit> kw_not_null, kw_auto_increment, kw_unique, kw_default, kw_create,
		kw_table;

	// Attributed tokens. (If you add a new type, don't forget to add it to the lex::lexertl::token definition too).
	lex::token_def<int> signed_digit;
	lex::token_def<std::size_t> unsigned_digit;
	lex::token_def<std::string> identifier;
	lex::token_def<std::string> quoted_string;

	sql_tokens()
	{
		// Column data types.
		type_smallint = "smallint";
		type_int = "int";
		type_varchar = "varchar";
		type_text = "text";
		type_date = "date";

		// Keywords.
		kw_not_null = "not null";
		kw_auto_increment = "auto_increment";
		kw_unique = "unique";
		kw_default = "default";
		kw_create = "create";
		kw_table = "table";

		// Values.
		signed_digit = "[+-]?[0-9]+";
		unsigned_digit = "[0-9]+";
		quoted_string = "\\\"(\\\\.|[^\\\"])*\\\""; // \"(\\.|[^\"])*\"

		// Identifier.
		identifier = "[a-zA-Z][a-zA-Z0-9_]*";

		// The token must be added in priority order.
		this->self += lex::token_def<>('(') | ')' | ',' | ';';
		this->self += type_smallint | type_int | type_varchar | type_text |
									type_date;
		this->self += kw_not_null | kw_auto_increment | kw_unique | kw_default |
									kw_create | kw_table;
		this->self += identifier | unsigned_digit | signed_digit | quoted_string;

		// define the whitespace to ignore.
		this->self("WS")
				=		lex::token_def<>("[ \\t\\n]+") 
				|		"--[^\\n]*\\n"  // Single line comments with --
				|		"\\/\\*[^*]*\\*+([^/*][^*]*\\*+)*\\/" // C-style comments
				;
	}
};

struct sql_column
{
	std::string column_identifier;
	boost::shared_ptr<sql::type::base_type> sql_type;
	std::vector<boost::shared_ptr<sql::base_type_constraint> > constraints;
};

BOOST_FUSION_ADAPT_STRUCT(
	sql_column,
	(std::string, column_identifier)
	(boost::shared_ptr<sql::type::base_type>, sql_type)
	(std::vector<boost::shared_ptr<sql::base_type_constraint> >, constraints)
)

struct sql_table
{
	std::string table_identifier;
	std::vector<sql_column> columns;
};

BOOST_FUSION_ADAPT_STRUCT(
	sql_table,
	(std::string, table_identifier)
	(std::vector<sql_column>, columns)
)

template <class synthesized, class inherited = void>
struct attribute
{
	typedef synthesized s_type;
	typedef inherited i_type;
	typedef s_type type(i_type);
};

template <class synthesized>
struct attribute <synthesized, void>
{
	typedef synthesized s_type;
	typedef s_type type();
};

template <class inherited>
struct attribute <void, inherited>
{
	typedef inherited i_type;
	typedef void type(i_type);
};

class semantic_actions
{
public:
	typedef attribute<boost::shared_ptr<sql::type::base_type> > column_type_attribute;
	typedef attribute<std::string> default_value_attribute;
	typedef attribute<boost::shared_ptr<sql::base_type_constraint> > type_constraint_attribute;
	typedef attribute<sql_column> column_attribute;
	typedef attribute<std::vector<sql_column> > create_table_columns_attribute;
	typedef attribute<sql_table> create_table_attribute;
	typedef attribute<sql_table> create_statement_attribute;
	typedef attribute<sql_table> statement_attribute;
	typedef attribute<std::vector<sql_table> > program_attribute;

	template<class T>
	void make_column_type(typename column_type_attribute::s_type& res) const
	{
		res = boost::make_shared<T>();
	}

	void make_varchar_type(typename column_type_attribute::s_type& res, std::size_t length) const
	{
		res = boost::make_shared<sql::type::varchar>(length);
	}

	template <class T>
	void make_type_constraint(typename type_constraint_attribute::s_type& res) const
	{
		res = boost::make_shared<T>();
	}

	void make_default_value_constraint(typename type_constraint_attribute::s_type& res, const std::string& default_value) const
	{
		res = boost::make_shared<sql::default_value>(default_value);
	}
};

// Grammar definition, define a little part of the SQL language.
template <typename Iterator, typename Lexer>
struct sql_grammar 
	: qi::grammar<Iterator, qi::in_state_skipper<Lexer>, typename semantic_actions::program_attribute::type>
{
	template <typename TokenDef>
	sql_grammar(TokenDef const& tok)
		: sql_grammar::base_type(program, "program")
	{
		program 
			%=  (statement % ';') >> *qi::lit(';')
			;

		statement 
			%=   create_statement
			;

		create_statement
			%=   tok.kw_create >> create_table
			;

		create_table
			%=	tok.kw_table >> tok.identifier >> '(' >> create_table_columns >> ')'
			;

		create_table_columns
			%=   column_definition % ','     // comma separated list of column_definition.
			;

		column_definition
			%=   tok.identifier >> column_type >> *type_constraint
			;

		type_constraint
			=   tok.kw_not_null		[phx::bind(&semantic_actions::make_type_constraint<sql::not_null>, &sa_, qi::_val)]
			|   tok.kw_auto_increment	[phx::bind(&semantic_actions::make_type_constraint<sql::auto_increment>, &sa_, qi::_val)]
			|   tok.kw_unique			[phx::bind(&semantic_actions::make_type_constraint<sql::unique>, &sa_, qi::_val)]
			|   default_value 		[phx::bind(&semantic_actions::make_default_value_constraint, &sa_, qi::_val, qi::_1)]
			;

		default_value
			%=   tok.kw_default > tok.quoted_string
			;

		column_type
			=   tok.type_smallint		[phx::bind(&semantic_actions::make_column_type<sql::type::smallint>, &sa_, qi::_val)]
			|   tok.type_int 				[phx::bind(&semantic_actions::make_column_type<sql::type::integer>, &sa_, qi::_val)]
			|   (tok.type_varchar > '(' > tok.unsigned_digit > ')') [phx::bind(&semantic_actions::make_varchar_type, &sa_, qi::_val, qi::_1)]
			|   tok.type_text 			[phx::bind(&semantic_actions::make_column_type<sql::type::text>, &sa_, qi::_val)]
			|   tok.type_date			  [phx::bind(&semantic_actions::make_column_type<sql::type::date>, &sa_, qi::_val)]
			;

		program.name("program");
		statement.name("statement");
		create_statement.name("create statement");
		create_table.name("create table");
		create_table_columns.name("create table columns");
		column_definition.name("column definition");
		column_type.name("column type");
		default_value.name("default value");
		type_constraint.name("type constraint");

		using namespace qi::labels;
		qi::on_error<qi::fail>
		(
			program,
			std::cout
				<< phx::val("Error! Expecting ")
				<< bs::_4                               // what failed?
				<< phx::val(" here: \"")
				<< phx::construct<std::string>(bs::_3, bs::_2)   // iterators to error-pos, end
				<< phx::val("\"")
				<< std::endl
		);
	}

private:
	typedef qi::in_state_skipper<Lexer> skipper_type;
	template <class Attribute>
	struct rule
	{
		typedef qi::rule<Iterator, skipper_type, Attribute> type;
	};
	typedef qi::rule<Iterator, skipper_type> simple_rule;

	semantic_actions sa_;

	//simple_rule program;
	typename rule<typename semantic_actions::program_attribute::type>::type program;
	typename rule<typename semantic_actions::statement_attribute::type>::type statement;
	typename rule<typename semantic_actions::create_statement_attribute::type>::type create_statement;
	typename rule<typename semantic_actions::create_table_attribute::type>::type create_table;
	typename rule<typename semantic_actions::create_table_columns_attribute::type>::type create_table_columns;
	typename rule<typename semantic_actions::column_attribute::type>::type column_definition;
	typename rule<typename semantic_actions::type_constraint_attribute::type>::type type_constraint;
	typename rule<typename semantic_actions::default_value_attribute::type>::type default_value;
	typename rule<typename semantic_actions::column_type_attribute::type>::type column_type;
};


int main(int argc, char* argv[])
{
	if(argc != 2)
		exit(1);

	// iterator type used to expose the underlying input stream
	typedef std::string::iterator base_iterator_type;

	// This is the lexer token type to use. The second template parameter lists 
	// all attribute types used for token_def's during token definition (see 
	// example5_base_tokens<> above). Here we use the predefined lexertl token 
	// type, but any compatible token type may be used instead.
	//
	// If you don't list any token attribute types in the following declaration 
	// (or just use the default token type: lexertl_token<base_iterator_type>)  
	// it will compile and work just fine, just a bit less efficient. This is  
	// because the token attribute will be generated from the matched input  
	// sequence every time it is requested. But as soon as you specify at 
	// least one token attribute type you'll have to list all attribute types 
	// used for token_def<> declarations in the token definition class above,  
	// otherwise compilation errors will occur.
	typedef lex::lexertl::token<
		base_iterator_type, boost::mpl::vector<int, std::size_t, std::string> 
	> token_type;

	// Here we use the lexertl based lexer engine.
	typedef lex::lexertl::lexer<token_type> lexer_type;

	// This is the token definition type (derived from the given lexer type).
	typedef sql_tokens<lexer_type> sql_tokens;

	// this is the iterator type exposed by the lexer 
	typedef sql_tokens::iterator_type iterator_type;

	// this is the type of the grammar to parse
	typedef sql_grammar<iterator_type, sql_tokens::lexer_def> sql_grammar;

	// now we use the types defined above to create the lexer and grammar
	// object instances needed to invoke the parsing process
	sql_tokens tokens;                         // Our lexer
	sql_grammar sql(tokens);                  // Our parser

	std::string str(argv[1]);

	// At this point we generate the iterator pair used to expose the
	// tokenized input stream.
	std::string::iterator it = str.begin();
	iterator_type iter = tokens.begin(it, str.end());
	iterator_type end = tokens.end();

	// Parsing is done based on the the token stream, not the character 
	// stream read from the input.
	// Note how we use the lexer defined above as the skip parser. It must
	// be explicitly wrapped inside a state directive, switching the lexer 
	// state for the duration of skipping whitespace.
	std::string ws("WS");
	typename semantic_actions::program_attribute::s_type sql_ast;
	bool r = qi::phrase_parse(iter, end, sql, qi::in_state(ws)[tokens.self], sql_ast);


	if (r && iter == end)
	{
		std::cout << "-------------------------\n";
		std::cout << "Parsing succeeded\n";
		std::cout << "-------------------------\n";
	}
	else
	{
		std::cout << "-------------------------\n";
		std::cout << "Parsing failed\n";
		std::cout << "-------------------------\n";
	}
	return 0;
}
