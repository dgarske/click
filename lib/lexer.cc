/*
 * lexer.{cc,hh} -- parses Click language files, produces Router objects
 * Eddie Kohler
 *
 * Copyright (c) 1999-2000 Massachusetts Institute of Technology
 * Copyright (c) 2000 Mazu Networks, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * Further elaboration of this license, including a DISCLAIMER OF ANY
 * WARRANTY, EXPRESS OR IMPLIED, is provided in the LICENSE file, which is
 * also accessible at http://www.pdos.lcs.mit.edu/click/license.html
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
#include <click/lexer.hh>
#include <click/router.hh>
#include <click/error.hh>
#include <click/confparse.hh>
#include <click/glue.hh>
#include <click/straccum.hh>
#include <click/variableenv.hh>
#include "elements/standard/errorelement.hh"

//
// CLASS LEXER::TUNNELEND
//

class Lexer::TunnelEnd {
  
  Router::Hookup _port;
  Vector<Router::Hookup> _correspond;
  int _expanded;
  bool _output;
  TunnelEnd *_other;
  TunnelEnd *_next;
  
 public:
  
  TunnelEnd(const Router::Hookup &, bool, TunnelEnd *);
  ~TunnelEnd()				{ delete _next; }
  
  const Router::Hookup &port() const	{ return _port; }
  bool output() const			{ return _output; }
  TunnelEnd *next() const		{ return _next; }
  void pair_with(TunnelEnd *d)		{ _other = d; d->_other = this; }
  
  TunnelEnd *find(const Router::Hookup &);
  void expand(const Lexer *, Vector<Router::Hookup> &);
  
};

//
// CLASS LEXER::SYNONYM
//

class Lexer::Synonym : public Element {
  
  Element *_element;
  
 public:
  
  Synonym(Element *e)			: _element(e) { }

  const char *class_name() const	{ return _element->class_name(); }
  void *cast(const char *);
  Element *clone() const		{ return _element->clone(); }
  
};

void *
Lexer::Synonym::cast(const char *s)
{
  if (strcmp(s, "Lexer::Synonym") == 0)
    return this;
  else
    return _element->cast(s);
}

//
// CLASS LEXER::COMPOUND
//

class Lexer::Compound : public Element {
  
  mutable String _name;
  String _landmark;
  int _depth;
  Element *_next;

  Vector<String> _formals;
  int _ninputs;
  int _noutputs;
  
  Vector<int> _elements;
  Vector<String> _element_names;
  Vector<String> _element_configurations;
  Vector<String> _element_landmarks;
  
  Vector<Hookup> _hookup_from;
  Vector<Hookup> _hookup_to;
  
 public:
  
  Compound(const String &, const String &, int, Element *);

  const String &name() const		{ return _name; }
  const String &landmark() const	{ return _landmark; }
  int nformals() const			{ return _formals.size(); }
  const Vector<String> &formals() const	{ return _formals; }
  void add_formal(const String &f)	{ _formals.push_back(f); }
  int depth() const			{ return _depth; }

  void swap_router(Lexer *);
  void finish(ErrorHandler *);
  void check_duplicates_until(Element *, ErrorHandler *);

  Element *find_relevant_class(int, int, const Vector<String> &);
  void report_signatures(ErrorHandler *);
  void expand_into(Lexer *, int, const VariableEnvironment &);
  
  const char *class_name() const	{ return _name.cc(); }
  void *cast(const char *);
  Compound *clone() const		{ return 0; }

  String signature() const;
  static String signature(const String &, int, int, int);
  
};

Lexer::Compound::Compound(const String &name, const String &lm, int depth, Element *next)
  : _name(name), _landmark(lm), _depth(depth), _next(next),
    _ninputs(0), _noutputs(0)
{
}

void *
Lexer::Compound::cast(const char *s)
{
  if (strcmp(s, "Lexer::Compound") == 0 || _name == s)
    return this;
  else
    return 0;
}

void
Lexer::Compound::swap_router(Lexer *lexer)
{
  lexer->_elements.swap(_elements);
  lexer->_element_names.swap(_element_names);
  lexer->_element_configurations.swap(_element_configurations);
  lexer->_element_landmarks.swap(_element_landmarks);

  lexer->_hookup_from.swap(_hookup_from);
  lexer->_hookup_to.swap(_hookup_to);
}

void
Lexer::Compound::finish(ErrorHandler *errh)
{
  assert(_element_names[0] == "input" && _element_names[1] == "output");

  // count numbers of inputs and outputs
  Vector<int> from_in, to_out;
  bool to_in = false, from_out = false;
  for (int i = 0; i < _hookup_from.size(); i++) {
    const Hookup &hf = _hookup_from[i], &ht = _hookup_to[i];
    
    if (hf.idx == 0) {
      if (from_in.size() <= hf.port)
	from_in.resize(hf.port + 1, 0);
      from_in[hf.port] = 1;
    } else if (hf.idx == 1)
      from_out = true;
    
    if (ht.idx == 1) {
      if (to_out.size() <= ht.port)
	to_out.resize(ht.port + 1, 0);
      to_out[ht.port] = 1;
    } else if (ht.idx == 0)
      to_in = true;
  }
  
  // store information
  _ninputs = from_in.size();
  if (to_in)
    errh->lerror(_landmark, "`%s' pseudoelement `input' may only be used as output", _name.cc());
  for (int i = 0; i < from_in.size(); i++)
    if (!from_in[i])
      errh->lerror(_landmark, "compound element `%s' input %d unused", _name.cc(), i);
  
  _noutputs = to_out.size();
  if (from_out)
    errh->lerror(_landmark, "`%s' pseudoelement `output' may only be used as input", _name.cc());
  for (int i = 0; i < to_out.size(); i++)
    if (!to_out[i])
      errh->lerror(_landmark, "compound element `%s' output %d unused", _name.cc(), i);
}

void
Lexer::Compound::check_duplicates_until(Element *last, ErrorHandler *errh)
{
  if (this == last || !_next)
    return;
  
  Element *n = _next;
  while (n && n != last) {
    Compound *nc = (Compound *)n->cast("Lexer::Compound");
    if (!nc) break;
    if (nc->_ninputs == _ninputs && nc->_noutputs == _noutputs && nc->_formals.size() == _formals.size()) {
      errh->lerror(_landmark, "redeclaration of `%s'", signature().cc());
      errh->lerror(nc->_landmark, "`%s' previously declared here", signature().cc());
      break;
    }
    n = nc->_next;
  }

  if (Compound *nc = (Compound *)_next->cast("Lexer::Compound"))
    nc->check_duplicates_until(last, errh);
}

Element *
Lexer::Compound::find_relevant_class(int ninputs, int noutputs, const Vector<String> &args)
{
  Compound *ct = this;
  
  while (1) {
    if (ct->_ninputs == ninputs && ct->_noutputs == noutputs && ct->_formals.size() == args.size())
      return ct;
    Element *e = ct->_next;
    if (!e)
      return 0;
    else if (Compound *next = (Compound *)e->cast("Lexer::Compound"))
      ct = next;
    else
      return e;
  }
}

String
Lexer::Compound::signature(const String &name, int ninputs, int noutputs, int nargs)
{
  StringAccum sa;
  const char *pl_args = (nargs == 1 ? " argument, " : " arguments, ");
  const char *pl_ins = (ninputs == 1 ? " input, " : " inputs,");
  const char *pl_outs = (noutputs == 1 ? " output" : " outputs");
  sa << name << '[' << nargs << pl_args << ninputs << pl_ins << noutputs << pl_outs << ']';
  return sa.take_string();
}

String
Lexer::Compound::signature() const
{
  return signature(_name, _ninputs, _noutputs, _formals.size());
}

void
Lexer::Compound::report_signatures(ErrorHandler *errh)
{
  if (_next) {
    if (Compound *n = (Compound *)_next->cast("Lexer::Compound"))
      n->report_signatures(errh);
    else
      errh->lmessage(_landmark, "`%s[...]'", _name.cc());
  }
  errh->lmessage(_landmark, "`%s'", signature().cc());
}

void
Lexer::Compound::expand_into(Lexer *lexer, int which, const VariableEnvironment &ve)
{
  ErrorHandler *errh = lexer->_errh;
  
  // `name_slash' is `name' constrained to end with a slash
  String ename = lexer->_element_names[which];
  String ename_slash;
  if (ename[ename.length() - 1] == '/')
    ename_slash = ename;
  else
    ename_slash = ename + "/";

  assert(_element_names[0] == "input" && _element_names[1] == "output");

  lexer->_elements[which] = TUNNEL_TYPE;
  lexer->add_tunnel(ename, ename_slash + "input");
  lexer->add_tunnel(ename_slash + "output", ename);

  Vector<int> eidx_map;
  eidx_map.push_back(lexer->_element_map[ename_slash + "input"]);
  eidx_map.push_back(lexer->_element_map[ename_slash + "output"]);

  for (int i = 2; i < _elements.size(); i++) {
    String cname = ename_slash + _element_names[i];
    int eidx = lexer->_element_map[cname];
    if (eidx >= 0) {
      errh->lerror(lexer->element_landmark(which), "redeclaration of element `%s'", cname.cc());
      errh->lerror(lexer->element_landmark(eidx), "`%s' previously declared here", cname.cc());
      eidx_map.push_back(-1);
    } else {
      if (lexer->_element_type_map[cname] >= 0)
	errh->lerror(lexer->element_landmark(which), "`%s' is an element class", cname.cc());
      eidx = lexer->get_element(cname, _elements[i], ve.interpolate(_element_configurations[i]), _element_landmarks[i]);
      eidx_map.push_back(eidx);
    }
  }

  // now copy hookups
  int nh = _hookup_from.size();
  for (int i = 0; i < nh; i++) {
    const Hookup &hf = _hookup_from[i], &ht = _hookup_to[i];
    if (eidx_map[hf.idx] >= 0 && eidx_map[ht.idx] >= 0)
      lexer->connect(eidx_map[hf.idx], hf.port, eidx_map[ht.idx], ht.port);
  }
}

//
// LEXER
//

Lexer::Lexer(ErrorHandler *errh)
  : _data(0), _len(0), _pos(0), _lineno(1), _lextra(0),
    _tpos(0), _tfull(0),
    _element_type_map(-1),
    _last_element_type(-1),
    _free_element_type(-1),
    _element_map(-1),
    _definputs(0), _defoutputs(0),
    _errh(errh)
{
  if (!_errh) _errh = ErrorHandler::default_handler();
  end_parse(-1);		// clear private state
  add_element_type("<tunnel>", new ErrorElement);
  add_element_type(new ErrorElement);
  assert(element_type("<tunnel>") == TUNNEL_TYPE && element_type("Error") == ERROR_TYPE);
}

Lexer::~Lexer()
{
  end_parse(-1);
}

int
Lexer::begin_parse(const String &data, const String &filename,
		   LexerExtra *lextra)
{
  _big_string = data;
  _data = _big_string.data();
  _len = _big_string.length();

  if (!filename)
    _filename = "line ";
  else if (filename.back() != ':' && !isspace(filename.back()))
    _filename = filename + ":";
  else
    _filename = filename;
  _original_filename = _filename;
  _lineno = 1;

  _lextra = lextra;
  return lexical_scoping_in();
}

void
Lexer::end_parse(int cookie)
{
  lexical_scoping_out(cookie);
  
  delete _definputs;
  _definputs = 0;
  delete _defoutputs;
  _defoutputs = 0;
  
  _elements.clear();
  _element_names.clear();
  _element_configurations.clear();
  _element_landmarks.clear();
  _element_map.clear();
  _hookup_from.clear();
  _hookup_to.clear();
  _requirements.clear();
  
  _big_string = "";
  // _data was freed by _big_string
  _data = 0;
  _len = 0;
  _pos = 0;
  _filename = "";
  _lineno = 0;
  _lextra = 0;
  _tpos = 0;
  _tfull = 0;
  
  _anonymous_offset = 0;
  _compound_depth = 0;
}


// LEXING: LOWEST LEVEL

unsigned
Lexer::skip_line(unsigned pos)
{
  _lineno++;
  for (; pos < _len; pos++)
    if (_data[pos] == '\n')
      return pos + 1;
    else if (_data[pos] == '\r') {
      if (pos < _len - 1 && _data[pos+1] == '\n')
	return pos + 2;
      else
	return pos + 1;
    }
  _lineno--;
  return _len;
}

unsigned
Lexer::skip_slash_star(unsigned pos)
{
  for (; pos < _len; pos++)
    if (_data[pos] == '\n')
      _lineno++;
    else if (_data[pos] == '\r') {
      if (pos < _len - 1 && _data[pos+1] == '\n') pos++;
      _lineno++;
    } else if (_data[pos] == '*' && pos < _len - 1 && _data[pos+1] == '/')
      return pos + 2;
  return _len;
}

unsigned
Lexer::skip_quote(unsigned pos, char endc)
{
  for (; pos < _len; pos++)
    if (_data[pos] == '\n')
      _lineno++;
    else if (_data[pos] == '\r') {
      if (pos < _len - 1 && _data[pos+1] == '\n') pos++;
      _lineno++;
    } else if (_data[pos] == '\\' && pos < _len - 1 && endc == '\"'
	       && _data[pos+1] == endc)
      pos++;
    else if (_data[pos] == endc)
      return pos + 1;
  return _len;
}

unsigned
Lexer::process_line_directive(unsigned pos)
{
  const char *data = _data;
  unsigned len = _len;
  
  for (pos++; pos < len && (data[pos] == ' ' || data[pos] == '\t'); pos++)
    /* nada */;
  if (pos < len - 4 && data[pos] == 'l' && data[pos+1] == 'i'
      && data[pos+2] == 'n' && data[pos+3] == 'e'
      && (data[pos+4] == ' ' || data[pos+4] == '\t')) {
    for (pos += 5; pos < len && (data[pos] == ' ' || data[pos] == '\t'); pos++)
      /* nada */;
  }
  if (pos >= len || !isdigit(data[pos])) {
    // complain about bad directive
    lerror("unknown preprocessor directive");
    return skip_line(pos);
  }
  
  // parse line number
  for (_lineno = 0; pos < len && isdigit(data[pos]); pos++)
    _lineno = _lineno * 10 + data[pos] - '0';
  _lineno--;			// account for extra line
  
  for (; pos < len && (data[pos] == ' ' || data[pos] == '\t'); pos++)
    /* nada */;
  if (pos < len && data[pos] == '\"') {
    // parse filename
    unsigned first_in_filename = pos;
    for (pos++; pos < len && data[pos] != '\"' && data[pos] != '\n' && data[pos] != '\r'; pos++)
      if (data[pos] == '\\' && pos < len - 1 && data[pos+1] != '\n' && data[pos+1] != '\r')
	pos++;
    _filename = cp_unquote(_big_string.substring(first_in_filename, pos - first_in_filename) + "\":");
    // an empty filename means return to the input file's name
    if (_filename == ":")
      _filename = _original_filename;
  }

  // reach end of line
  for (; pos < len && data[pos] != '\n' && data[pos] != '\r'; pos++)
    /* nada */;
  if (pos < len - 1 && data[pos] == '\r' && data[pos+1] == '\n')
    pos++;
  return pos;
}

Lexeme
Lexer::next_lexeme()
{
  unsigned pos = _pos;
  while (true) {
    while (pos < _len && isspace(_data[pos])) {
      if (_data[pos] == '\n')
	_lineno++;
      else if (_data[pos] == '\r') {
	if (pos < _len - 1 && _data[pos+1] == '\n') pos++;
	_lineno++;
      }
      pos++;
    }
    if (pos >= _len) {
      _pos = _len;
      return Lexeme();
    } else if (_data[pos] == '/' && pos < _len - 1) {
      if (_data[pos+1] == '/')
	pos = skip_line(pos + 2);
      else if (_data[pos+1] == '*')
	pos = skip_slash_star(pos + 2);
      else
	break;
    } else if (_data[pos] == '#' && (pos == 0 || _data[pos-1] == '\n' || _data[pos-1] == '\r'))
      pos = process_line_directive(pos);
    else
      break;
  }
  
  unsigned word_pos = pos;
  
  // find length of current word
  if (isalnum(_data[pos]) || _data[pos] == '_' || _data[pos] == '@') {
    pos++;
    while (pos < _len && (isalnum(_data[pos]) || _data[pos] == '_'
			  || _data[pos] == '/' || _data[pos] == '@')) {
      if (_data[pos] == '/' && pos < _len - 1
	  && (_data[pos+1] == '/' || _data[pos+1] == '*'))
	break;
      pos++;
    }
    _pos = pos;
    String word = _big_string.substring(word_pos, pos - word_pos);
    if (word.length() == 16 && word == "connectiontunnel")
      return Lexeme(lexTunnel, word);
    else if (word.length() == 12 && word == "elementclass")
      return Lexeme(lexElementclass, word);
    else if (word.length() == 7 && word == "require")
      return Lexeme(lexRequire, word);
    else
      return Lexeme(lexIdent, word);
  }

  // check for variable
  if (_data[pos] == '$') {
    pos++;
    while (pos < _len && (isalnum(_data[pos]) || _data[pos] == '_'))
      pos++;
    if (pos > word_pos + 1) {
      _pos = pos;
      return Lexeme(lexVariable, _big_string.substring(word_pos, pos - word_pos));
    } else
      pos--;
  }
  
  if (pos < _len - 1) {
    if (_data[pos] == '-' && _data[pos+1] == '>') {
      _pos = pos + 2;
      return Lexeme(lexArrow, _big_string.substring(word_pos, 2));
    } else if (_data[pos] == ':' && _data[pos+1] == ':') {
      _pos = pos + 2;
      return Lexeme(lex2Colon, _big_string.substring(word_pos, 2));
    } else if (_data[pos] == '|' && _data[pos+1] == '|') {
      _pos = pos + 2;
      return Lexeme(lex2Bar, _big_string.substring(word_pos, 2));
    }
  }
  if (pos < _len - 2 && _data[pos] == '.' && _data[pos+1] == '.' && _data[pos+2] == '.') {
    _pos = pos + 3;
    return Lexeme(lex3Dot, _big_string.substring(word_pos, 3));
  }
  
  _pos = pos + 1;
  return Lexeme(_data[word_pos], _big_string.substring(word_pos, 1));
}

String
Lexer::lex_config()
{
  unsigned config_pos = _pos;
  unsigned pos = _pos;
  unsigned paren_depth = 1;
  int quote = 0;
  String output;
  
  for (; pos < _len; pos++)
    if (_data[pos] == '(' && !quote)
      paren_depth++;
    else if (_data[pos] == ')' && !quote) {
      paren_depth--;
      if (!paren_depth) break;
    } else if (_data[pos] == '\n')
      _lineno++;
    else if (_data[pos] == '\r') {
      if (pos < _len - 1 && _data[pos+1] == '\n') pos++;
      _lineno++;
    } else if (_data[pos] == '/' && pos < _len - 1 && !quote) {
      if (_data[pos+1] == '/')
	pos = skip_line(pos + 2) - 1;
      else if (_data[pos+1] == '*')
	pos = skip_slash_star(pos + 2) - 1;
    } else if ((_data[pos] == '\'' || _data[pos] == '\"') && !quote)
      quote = _data[pos];
    else if (quote && _data[pos] == quote)
      quote = 0;
    else if (_data[pos] == '\\' && pos < _len - 1 && quote == '\"') {
      if (_data[pos+1] == '\"' || _data[pos+1] == '$')
	pos++;
    }
  
  _pos = pos;
  if (!output)
    return _big_string.substring(config_pos, pos - config_pos);
  else
    return output + _big_string.substring(config_pos, pos - config_pos);
}

String
Lexer::lexeme_string(int kind)
{
  char buf[12];
  if (kind == lexIdent)
    return "identifier";
  else if (kind == lexIdent)
    return "variable";
  else if (kind == lexArrow)
    return "`->'";
  else if (kind == lex2Colon)
    return "`::'";
  else if (kind == lex2Bar)
    return "`||'";
  else if (kind == lex3Dot)
    return "`...'";
  else if (kind == lexTunnel)
    return "`connectiontunnel'";
  else if (kind == lexElementclass)
    return "`elementclass'";
  else if (kind == lexRequire)
    return "`require'";
  else if (kind >= 32 && kind < 127) {
    sprintf(buf, "`%c'", kind);
    return buf;
  } else {
    sprintf(buf, "`\\%03d'", kind);
    return buf;
  }
}


// LEXING: MIDDLE LEVEL (WITH PUSHBACK)

const Lexeme &
Lexer::lex()
{
  int p = _tpos;
  if (_tpos == _tfull) {
    _tcircle[p] = next_lexeme();
    _tfull = (_tfull + 1) % TCIRCLE_SIZE;
  }
  _tpos = (_tpos + 1) % TCIRCLE_SIZE;
  return _tcircle[p];
}

void
Lexer::unlex(const Lexeme &t)
{
  _tcircle[_tfull] = t;
  _tfull = (_tfull + 1) % TCIRCLE_SIZE;
  assert(_tfull != _tpos);
}

bool
Lexer::expect(int kind, bool report_error = true)
{
  const Lexeme &t = lex();
  if (t.is(kind))
    return true;
  else {
    if (report_error)
      lerror("expected %s", lexeme_string(kind).cc());
    unlex(t);
    return false;
  }
}


// ERRORS

String
Lexer::landmark() const
{
  return _filename + String(_lineno);
}

int
Lexer::lerror(const char *format, ...)
{
  va_list val;
  va_start(val, format);
  _errh->verror(ErrorHandler::Error, landmark(), format, val);
  va_end(val);
  return -1;
}


// ELEMENT TYPES

int
Lexer::add_element_type(Element *e)
{
  // Lexer now owns `e'
  return add_element_type("", e);
}

int
Lexer::add_element_type(String name, Element *e)
{
  assert(e);
  // Lexer now owns `e'
  int tid;
  if (!name)
    name = e->class_name();
  if (_free_element_type < 0) {
    tid = _element_types.size();
    _element_types.push_back(e);
    _element_type_names.push_back(name);
    _element_type_next.push_back(_last_element_type);
  } else {
    tid = _free_element_type;
    _free_element_type = _element_type_next[tid];
    _element_types[tid] = e;
    _element_type_names[tid] = name;
    _element_type_next[tid] = _last_element_type;
  }
  _element_type_map.insert(name, tid);
  _last_element_type = tid;
  return tid;
}

int
Lexer::element_type(const String &s) const
{
  return _element_type_map[s];
}

int
Lexer::force_element_type(String s)
{
  int ftid = _element_type_map[s];
  if (ftid >= 0)
    return ftid;
  lerror("unknown element class `%s'", s.cc());
  return add_element_type(s, new ErrorElement);
}

int
Lexer::lexical_scoping_in() const
{
  return _last_element_type;
}

void
Lexer::lexical_scoping_out(int last)
{
  if (last != -1) {
    for (int t = _last_element_type; t >= 0; t = _element_type_next[t])
      if (t == last)
	goto ok;
    return;
  }

 ok:
  while (_last_element_type != last)
    remove_element_type(_last_element_type);
}

void
Lexer::remove_element_type(int removed)
{
  // patch next array
  int prev = -1, trav = _last_element_type;
  while (trav != removed && trav >= 0) {
    prev = trav;
    trav = _element_type_next[trav];
  }
  if (trav < 0)
    return;
  if (prev >= 0)
    _element_type_next[prev] = _element_type_next[removed];
  else
    _last_element_type = _element_type_next[removed];

  // fix up element type name map
  const String &name = _element_type_names[removed];
  if (_element_type_map[name] == removed) {
    for (trav = _element_type_next[removed];
	 trav >= 0 && _element_type_names[trav] != name;
	 trav = _element_type_next[trav])
      /* nada */;
    _element_type_map.insert(name, trav);
  }

  // remove stuff
  _element_type_names[removed] = String();
  delete _element_types[removed];
  _element_types[removed] = 0;
  _element_type_next[removed] = _free_element_type;
  _free_element_type = removed;
}

void
Lexer::element_type_names(Vector<String> &v) const
{
  for (HashMap<String, int>::Iterator i = _element_type_map.first(); i; i++)
    if (i.value() >= 0 && i.key() != "<tunnel>")
      v.push_back(i.key());
}


// PORT TUNNELS

void
Lexer::add_tunnel(String namein, String nameout)
{
  Hookup hin(get_element(namein, TUNNEL_TYPE), 0);
  Hookup hout(get_element(nameout, TUNNEL_TYPE), 0);
  
  bool ok = true;
  if (_elements[hin.idx] != TUNNEL_TYPE) {
    lerror("redeclaration of element `%s'", namein.cc());
    _errh->lerror(_element_landmarks[hin.idx], "`%s' previously declared here", namein.cc());
    ok = 0;
  }
  if (_elements[hout.idx] != TUNNEL_TYPE) {
    lerror("redeclaration of element `%s'", nameout.cc());
    _errh->lerror(_element_landmarks[hout.idx], "`%s' previously declared here", nameout.cc());
    ok = 0;
  }
  if (_definputs && _definputs->find(hin))
    lerror("redeclaration of connection tunnel input `%s'", namein.cc()), ok = 0;
  if (_defoutputs && _defoutputs->find(hout))
    lerror("redeclaration of connection tunnel output `%s'", nameout.cc()), ok = 0;
  if (ok) {
    _definputs = new TunnelEnd(hin, false, _definputs);
    _defoutputs = new TunnelEnd(hout, true, _defoutputs);
    _definputs->pair_with(_defoutputs);
  }
}

// ELEMENTS

int
Lexer::get_element(String name, int etype, const String &conf,
		   const String &lm)
{
  assert(name && etype >= 0 && etype < _element_types.size());
  
  // if an element `name' already exists return it
  if (_element_map[name] >= 0)
    return _element_map[name];

  int eid = _elements.size();
  _element_map.insert(name, eid);
  _element_names.push_back(name);
  _element_configurations.push_back(conf);
  _element_landmarks.push_back(lm ? lm : landmark());
  _elements.push_back(etype);
  return eid;
}

String
Lexer::anon_element_name(const String &class_name) const
{
  String prefix = class_name + "@";
  int anonymizer = _elements.size() - _anonymous_offset + 1;
  String name = prefix + String(anonymizer);
  while (_element_map[name] >= 0) {
    anonymizer++;
    name = prefix + String(anonymizer);
  }
  return name;
}

String
Lexer::anon_element_class_name(String prefix) const
{
  int anonymizer = _elements.size() - _anonymous_offset + 1;
  String name = prefix + String(anonymizer);
  while (_element_type_map[name] >= 0) {
    anonymizer++;
    name = prefix + String(anonymizer);
  }
  return name;
}

void
Lexer::connect(int element1, int port1, int element2, int port2)
{
  if (port1 < 0) port1 = 0;
  if (port2 < 0) port2 = 0;
  _hookup_from.push_back(Router::Hookup(element1, port1));
  _hookup_to.push_back(Router::Hookup(element2, port2));
}

String
Lexer::element_name(int eid) const
{
  if (eid < 0 || eid >= _elements.size())
    return "##no-such-element##";
  else if (_element_names[eid])
    return _element_names[eid];
  else {
    char buf[100];
    sprintf(buf, "@%d", eid);
    int t = _elements[eid];
    if (t == TUNNEL_TYPE)
      return "<tunnel" + String(buf) + ">";
    else if (!_element_types[t])
      return "<null" + String(buf) + ">";
    else
      return _element_types[t]->class_name() + String(buf);
  }
}

String
Lexer::element_landmark(int eid) const
{
  if (eid < 0 || eid >= _elements.size())
    return "##no-such-element##";
  else if (_element_landmarks[eid])
    return _element_landmarks[eid];
  else
    return "<unknown>";
}


// PARSING

bool
Lexer::yport(int &port)
{
  const Lexeme &tlbrack = lex();
  if (!tlbrack.is('[')) {
    unlex(tlbrack);
    return false;
  }
  
  const Lexeme &tword = lex();
  if (tword.is(lexIdent)) {
    if (!cp_integer(tword.string(), &port)) {
      lerror("syntax error: port number should be integer");
      port = 0;
    }
    expect(']');
    return true;
  } else if (tword.is(']')) {
    lerror("syntax error: expected port number");
    port = 0;
    return true;
  } else {
    lerror("syntax error: expected port number");
    unlex(tword);
    return false;
  }
}

bool
Lexer::yelement(int &element, bool comma_ok)
{
  Lexeme t = lex();
  String name;
  int etype;

  if (t.is(lexIdent)) {
    name = t.string();
    etype = element_type(name);
  } else if (t.is('{')) {
    etype = ycompound();
    name = _element_types[etype]->class_name();
  } else {
    unlex(t);
    return false;
  }

  String configuration, lm;
  const Lexeme &tparen = lex();
  if (tparen.is('(')) {
    lm = landmark();		// report landmark from before config string
    if (etype < 0) etype = force_element_type(name);
    configuration = lex_config();
    expect(')');
  } else
    unlex(tparen);
  
  if (etype >= 0)
    element = get_element(anon_element_name(name), etype, configuration, lm);
  else {
    element = _element_map[name];
    if (element < 0) {
      const Lexeme &t2colon = lex();
      unlex(t2colon);
      if (t2colon.is(lex2Colon) || (t2colon.is(',') && comma_ok))
	ydeclaration(name);
      else {
	lerror("undeclared element `%s' (first use this block)", name.cc());
	get_element(name, ERROR_TYPE);
      }
      element = _element_map[name];
    }
  }
  
  return true;
}

void
Lexer::ydeclaration(const String &first_element)
{
  Vector<String> decls;
  Lexeme t;

  if (first_element) {
    decls.push_back(first_element);
    goto midpoint;
  }
  
  while (true) {
    t = lex();
    if (!t.is(lexIdent))
      lerror("syntax error: expected element name");
    else
      decls.push_back(t.string());
    
   midpoint:
    const Lexeme &tsep = lex();
    if (tsep.is(','))
      /* do nothing */;
    else if (tsep.is(lex2Colon))
      break;
    else {
      lerror("syntax error: expected `::' or `,'");
      unlex(tsep);
      return;
    }
  }

  String lm = landmark();
  int etype;
  t = lex();
  if (t.is(lexIdent))
    etype = force_element_type(t.string());
  else if (t.is('{'))
    etype = ycompound();
  else {
    lerror("missing element type in declaration");
    return;
  }
  
  String configuration;
  t = lex();
  if (t.is('(')) {
    configuration = lex_config();
    expect(')');
  } else
    unlex(t);

  for (int i = 0; i < decls.size(); i++) {
    String name = decls[i];
    if (_element_map[name] >= 0) {
      int e = _element_map[name];
      lerror("redeclaration of element `%s'", name.cc());
      if (_elements[e] != TUNNEL_TYPE)
	_errh->lerror(_element_landmarks[e], "element `%s' previously declared here", name.cc());
    } else if (_element_type_map[name] >= 0)
      lerror("`%s' is an element class", name.cc());
    else
      get_element(name, etype, configuration, lm);
  }
}

bool
Lexer::yconnection()
{
  int element1 = -1;
  int port1;
  Lexeme t;
  
  while (true) {
    int element2;
    int port2 = -1;
    
    // get element
    yport(port2);
    if (!yelement(element2, element1 < 0)) {
      if (port1 >= 0)
	lerror("output port useless at end of chain");
      return element1 >= 0;
    }
    
    if (element1 >= 0)
      connect(element1, port1, element2, port2);
    else if (port2 >= 0)
      lerror("input port useless at start of chain");
    
    port1 = -1;
    
   relex:
    t = lex();
    switch (t.kind()) {
      
     case ',':
     case lex2Colon:
      lerror("syntax error before `%s'", t.string().cc());
      goto relex;
      
     case lexArrow:
      break;
      
     case '[':
      unlex(t);
      yport(port1);
      goto relex;
      
     case lexIdent:
     case '{':
     case '}':
     case lex2Bar:
     case lexTunnel:
     case lexElementclass:
     case lexRequire:
      unlex(t);
      // FALLTHRU
     case ';':
     case lexEOF:
      if (port1 >= 0)
	lerror("output port useless at end of chain", port1);
      return true;
      
     default:
      lerror("syntax error near `%s'", t.string().cc());
      if (t.kind() >= lexIdent)	// save meaningful tokens
	unlex(t);
      return true;
      
    }
    
    // have `x ->'
    element1 = element2;
  }
}

void
Lexer::yelementclass()
{
  Lexeme tname = lex();
  String name;
  if (tname.is(lexIdent))
    name = tname.string();
  else {
    unlex(tname);
    lerror("expected element type name");
  }

  Lexeme tnext = lex();
  if (tnext.is('{'))
    ycompound(name);
    
  else if (tnext.is(lexIdent)) {
    // define synonym type
    int t = force_element_type(tnext.string());
    Element *et = _element_types[t];
    add_element_type(name, new Synonym(et));

  } else {
    lerror("syntax error near `%s'", String(tnext.string()).cc());
    add_element_type(name, new ErrorElement);
  }
}

void
Lexer::ytunnel()
{
  while (true) {
    Lexeme tname1 = lex();
    if (!tname1.is(lexIdent)) {
      unlex(tname1);
      lerror("expected port name");
    }
    
    expect(lexArrow);
    
    Lexeme tname2 = lex();
    if (!tname2.is(lexIdent)) {
      unlex(tname2);
      lerror("expected port name");
    }
    
    if (tname1.is(lexIdent) && tname2.is(lexIdent))
      add_tunnel(tname1.string(), tname2.string());
    
    const Lexeme &t = lex();
    if (!t.is(',')) {
      unlex(t);
      return;
    }
  }
}

void
Lexer::ycompound_arguments(Compound *comptype)
{
  while (1) {
    const Lexeme &tvar = lex();
    if (!tvar.is(lexVariable)) {
      unlex(tvar);
      return;
    }
    comptype->add_formal(tvar.string());
    const Lexeme &tsep = lex();
    if (tsep.is('|'))
      return;
    else if (!tsep.is(',')) {
      lerror("expected `,' or `|'");
      unlex(tsep);
      return;
    }
  }
}

int
Lexer::ycompound(String name)
{
  if (!name)
    name = anon_element_class_name("@Class");

  HashMap<String, int> old_element_map(-1);
  old_element_map.swap(_element_map);
  HashMap<String, int> old_type_map(_element_type_map);
  int old_offset = _anonymous_offset;
  Element *created = 0;

  // check for '...'
  const Lexeme &t = lex();
  if (t.is(lex3Dot)) {
    if (_element_type_map[name] < 0) {
      lerror("extending unknwon element class `%s'", name.cc());
      add_element_type(name, new ErrorElement);
    }
    created = _element_types[ _element_type_map[name] ];
    expect(lex2Bar);
  } else
    unlex(t);

  Element *first = created;

  while (1) {
    // prepare
    _element_map.clear();
    Compound *ct = new Compound(name, landmark(), _compound_depth, created);
    ct->swap_router(this);
    get_element("input", TUNNEL_TYPE);
    get_element("output", TUNNEL_TYPE);
    _anonymous_offset = 2;
    _compound_depth++;

    ycompound_arguments(ct);
    while (ystatement(true))
      /* nada */;

    _compound_depth--;
    _anonymous_offset = old_offset;
    old_type_map.swap(_element_type_map);
    ct->swap_router(this);

    ct->finish(_errh);
    created = ct;

    // check for '||' or '}'
    const Lexeme &t = lex();
    if (!t.is(lex2Bar))
      break;
  }
  
  // on the way out
  ((Compound *)created)->check_duplicates_until(first, _errh);
  old_element_map.swap(_element_map);
  
  return add_element_type(name, created);
}

void
Lexer::yrequire()
{
  if (expect('(')) {
    String requirement = lex_config();
    Vector<String> args;
    cp_argvec(requirement, args);
    String word;
    for (int i = 0; i < args.size(); i++) {
      if (!cp_word(args[i], &word))
	lerror("bad requirement: should be a single word");
      else {
	if (_lextra)
	  _lextra->require(word, _errh);
	_requirements.push_back(word);
      }
    }
    expect(')');
  }
}

bool
Lexer::ystatement(bool nested)
{
  const Lexeme &t = lex();
  switch (t.kind()) {
    
   case lexIdent:
   case '[':
   case '{':
    unlex(t);
    yconnection();
    return true;
    
   case lexElementclass:
    yelementclass();
    return true;
    
   case lexTunnel:
    ytunnel();
    return true;

   case lexRequire:
    yrequire();
    return true;

   case ';':
    return true;
    
   case '}':
   case lex2Bar:
    if (!nested)
      goto syntax_error;
    unlex(t);
    return false;
    
   case lexEOF:
    if (nested)
      lerror("expected `}'");
    return false;
    
   default:
   syntax_error:
    lerror("syntax error near `%s'", String(t.string()).cc());
    return true;
    
  }
}


// COMPLETION

void
Lexer::add_router_connections(int c, const Vector<int> &router_id,
			      Router *router)
{
  Vector<Hookup> hfrom;
  expand_connection(_hookup_from[c], true, hfrom);
  Vector<Hookup> hto;
  expand_connection(_hookup_to[c], false, hto);
  for (int f = 0; f < hfrom.size(); f++) {
    int eidx = router_id[hfrom[f].idx];
    if (eidx >= 0)
      for (int t = 0; t < hto.size(); t++) {
	int tidx = router_id[hto[t].idx];
	if (tidx >= 0)
	  router->add_connection(eidx, hfrom[f].port, tidx, hto[t].port);
      }
  }
}

void
Lexer::expand_compound_element(int which, Vector<int> &environment_map, Vector<VariableEnvironment *> &environments)
{
  String name = _element_names[which];
  int etype = _elements[which];
  assert(name);
  int old_nelements = _elements.size();
    
  // find right version
  Vector<String> args;
  cp_argvec(_element_configurations[which], args);
  int inputs_used = 0, outputs_used = 0;
  for (int i = 0; i < _hookup_from.size(); i++) {
    const Hookup &hf = _hookup_from[i], &ht = _hookup_to[i];
    if (ht.idx == which && ht.port >= inputs_used)
      inputs_used = ht.port + 1;
    if (hf.idx == which && hf.port >= outputs_used)
      outputs_used = hf.port + 1;
  }
  Compound *c = (Compound *)_element_types[etype];
  Element *found_c = c->find_relevant_class(inputs_used, outputs_used, args);
  if (!found_c) {
    _errh->lerror(c->landmark(), "no match for `%s'", Compound::signature(c->name(), inputs_used, outputs_used, args.size()).cc());
    ContextErrorHandler cerrh(_errh, "possibilities are:", "  ");
    c->report_signatures(&cerrh);
    _elements[which] = ERROR_TYPE;
    return;
  }

  // have putatively correct version
  Compound *found_comp = (Compound *)found_c->cast("Lexer::Compound");
  if (!found_comp) {
    for (int i = 0; i < _element_types.size(); i++)
      if (_element_types[i] == found_c) {
	_elements[which] = i;
	return;
      }
    assert(0);
    _elements[which] = ERROR_TYPE;
    return;
  }
  
  int vei = environment_map[which];
  if (args.size() == 0 && found_comp->depth() == 0)
    vei = 0;
  else if (args.size() || environments[vei]->depth() >= found_comp->depth()) {
    VariableEnvironment *new_ve = new VariableEnvironment;
    if (vei > 0)
      new_ve->enter(*environments[vei]);
    new_ve->limit_depth(found_comp->depth());
    new_ve->enter(found_comp->formals(), args, found_comp->depth());
    environments.push_back(new_ve);
    vei = environments.size() - 1;
  }

  found_comp->expand_into(this, which, *environments[vei]);

  // mark new environments
  for (int i = old_nelements; i < _elements.size(); i++)
    environment_map.push_back(vei);
}

Router *
Lexer::create_router()
{
  Router *router = new Router;
  if (!router)
    return 0;
  
  // expand compounds
  Vector<int> environment_map(_elements.size(), 0);
  Vector<VariableEnvironment *> environments;
  environments.push_back(new VariableEnvironment);
  
  for (int i = 0; i < _elements.size(); i++) {
    int t = _elements[i];
    if (t != TUNNEL_TYPE && _element_types[t]->cast("Lexer::Compound"))
      expand_compound_element(i, environment_map, environments);
  }

  for (int i = 0; i < environments.size(); i++)
    delete environments[i];
  
  // add elements to router
  Vector<int> router_id;
  for (int i = 0; i < _elements.size(); i++)
    if (_elements[i] != TUNNEL_TYPE) {
      Element *tc = _element_types[_elements[i]];
      int ei = router->add_element
	(tc->clone(), _element_names[i], _element_configurations[i], _element_landmarks[i]);
      router_id.push_back(ei);
    } else
      router_id.push_back(-1);
  
  // add connections to router
  for (int i = 0; i < _hookup_from.size(); i++) {
    int fromi = router_id[ _hookup_from[i].idx ];
    int toi = router_id[ _hookup_to[i].idx ];
    if (fromi >= 0 && toi >= 0)
      router->add_connection(fromi, _hookup_from[i].port,
			     toi, _hookup_to[i].port);
    else
      add_router_connections(i, router_id, router);
  }

  // add requirements to router
  for (int i = 0; i < _requirements.size(); i++)
    router->add_requirement(_requirements[i]);

  return router;
}


//
// LEXEREXTRA
//

void
LexerExtra::require(String, ErrorHandler *)
{
}


//
// LEXER::TUNNELEND RELATED STUFF
//

Lexer::TunnelEnd::TunnelEnd(const Router::Hookup &port, bool output,
			    TunnelEnd *next)
  : _port(port), _expanded(0), _output(output), _other(0), _next(next)
{
  assert(!next || next->_output == _output);
}

Lexer::TunnelEnd *
Lexer::TunnelEnd::find(const Router::Hookup &h)
{
  TunnelEnd *d = this;
  TunnelEnd *parent = 0;
  while (d) {
    if (d->_port == h)
      return d;
    else if (d->_port.idx == h.idx)
      parent = d;
    d = d->_next;
  }
  // didn't find the particular port pair; make a new one if possible
  if (parent) {
    Hookup other(parent->_other->_port.idx, h.port);
    TunnelEnd *new_me = new TunnelEnd(h, _output, parent->_next);
    TunnelEnd *new_other = new TunnelEnd(other, !_output, parent->_other->_next);
    new_me->pair_with(new_other);
    parent->_next = new_me;
    parent->_other->_next = new_other;
    return new_me;
  } else
    return 0;
}

void
Lexer::TunnelEnd::expand(const Lexer *lexer, Vector<Router::Hookup> &into)
{
  if (_expanded == 1)
    return;
  
  if (_expanded == 0) {
    _expanded = 1;
    
    Vector<Router::Hookup> connections;
    lexer->find_connections(_other->_port, !_output, connections);

    // give good errors for unused or nonexistent compound element ports
    if (!connections.size()) {
      Hookup inh = (_output ? _other->_port : _port);
      Hookup outh = (_output ? _port : _other->_port);
      String in_name = lexer->element_name(inh.idx);
      String out_name = lexer->element_name(outh.idx);
      if (in_name + "/input" == out_name) {
	const char *message = (_output ? "`%s' input %d unused"
			       : "`%s' has no input %d");
	lexer->errh()->lerror(lexer->element_landmark(inh.idx), message,
			      in_name.cc(), inh.port);
      } else if (in_name == out_name + "/output") {
	const char *message = (_output ? "`%s' has no output %d"
			       : "`%s' output %d unused");
	lexer->errh()->lerror(lexer->element_landmark(outh.idx), message,
			      out_name.cc(), outh.port);
      } else {
	lexer->errh()->lerror(lexer->element_landmark(_other->_port.idx),
			      "tunnel `%s -> %s' %s %d unused",
			      in_name.cc(), out_name.cc(),
			      (_output ? "input" : "output"), _port.idx);
      }
    }

    for (int i = 0; i < connections.size(); i++)
      lexer->expand_connection(connections[i], _output, _correspond);
    
    _expanded = 2;
  }
  
  for (int i = 0; i < _correspond.size(); i++)
    into.push_back(_correspond[i]);
}

void
Lexer::find_connections(const Hookup &this_end, bool is_out,
			Vector<Hookup> &into) const
{
  const Vector<Hookup> &hookup_this(is_out ? _hookup_from : _hookup_to);
  const Vector<Hookup> &hookup_that(is_out ? _hookup_to : _hookup_from);
  for (int i = 0; i < hookup_this.size(); i++)
    if (hookup_this[i] == this_end)
      into.push_back(hookup_that[i]);
}

void
Lexer::expand_connection(const Hookup &this_end, bool is_out,
			 Vector<Hookup> &into) const
{
  if (_elements[this_end.idx] != TUNNEL_TYPE)
    into.push_back(this_end);
  else {
    TunnelEnd *dp = (is_out ? _defoutputs : _definputs);
    if (dp)
      dp = dp->find(this_end);
    if (dp)
      dp->expand(this, into);
    else if ((dp = (is_out ? _definputs : _defoutputs)->find(this_end)))
      _errh->lerror(_element_landmarks[this_end.idx],
		    (is_out ? "`%s' used as output" : "`%s' used as input"),
		    element_name(this_end.idx).cc());
  }
}
