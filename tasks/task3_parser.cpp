#include "task3_parser.h"

#include "task5_pipes.h"

#include <cctype>
#include <cstdlib>
#include <glob.h>
#include <unistd.h>

struct Tok {
  string text;
  bool glob = false;
  bool op = false;
  bool tilde = true;
};

size_t find_paren_end(const string &s, size_t open) {
  int depth = 1;
  char quote = 0, backtick = 0;

  for (size_t i = open + 1; i < s.size(); i++) {
    char c = s[i];

    if (quote == '\'') {
      if (c == '\'') {
        quote = 0;
      }
      continue;
    }

    if (quote == '"') {
      if (c == '"') {
        quote = 0;
      } else if (c == '\\') {
        i++;
      }
      continue;
    }

    if (backtick) {
      if (c == '`') {
        backtick = 0;
      } else if (c == '\\') {
        i++;
      }
      continue;
    }

    if (c == '\'') {
      quote = c;
    } else if (c == '"') {
      quote = c;
    } else if (c == '`') {
      backtick = 1;
    } else if (c == '\\') {
      i++;
    } else if (c == '(') {
      depth++;
    } else if (c == ')') {
      depth--;
      if (depth == 0) {
        return i;
      }
    }
  }

  return string::npos;
}

string get_var(const string &name, Tab &tab) {
  if (name == "?") {
    return to_string(tab.last_status);
  }

  if (name == "$") {
    return to_string(getpid());
  }

  const char *v = getenv(name.c_str());
  return v ? v : "";
}

vector<string> split_ws(const string &s) {
  vector<string> parts;
  string cur;

  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (!cur.empty()) {
        parts.push_back(cur);
        cur.clear();
      }
    } else {
      cur += c;
    }
  }

  if (!cur.empty()) {
    parts.push_back(cur);
  }

  return parts;
}

string strip_trailing_newlines(string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
    s.pop_back();
  }
  return s;
}

string capture_output(const string &line, Tab &tab) {
  vector<Command> cmds;
  string error;

  if (!parse_line(line, tab, cmds, error)) {
    return "";
  }

  string out;
  execute_pipeline(tab, cmds, &out);
  return strip_trailing_newlines(out);
}

bool parse_line(const string &line, Tab &tab, vector<Command> &cmds, string &error) {
  vector<Tok> toks;
  Tok cur;
  bool in_tok = false;
  char quote = 0;

  auto start_tok = [&](bool tilde_ok) {
    if (!in_tok) {
      cur = Tok();
      cur.tilde = tilde_ok;
      in_tok = true;
    }
  };

  auto end_tok = [&]() {
    if (in_tok) {
      toks.push_back(cur);
      cur = Tok();
      in_tok = false;
    }
  };

  auto add_expansion = [&](const string &value, bool quoted) {
    if (quoted) {
      start_tok(false);
      cur.text += value;
      return;
    }

    vector<string> parts = split_ws(value);
    for (size_t k = 0; k < parts.size(); k++) {
      if (k > 0) {
        end_tok();
      }
      start_tok(false);
      cur.text += parts[k];
    }
  };

  size_t i = 0;
  while (i < line.size()) {
    char c = line[i];

    if (quote == '\'') {
      if (c == '\'') {
        quote = 0;
      } else {
        cur.text += c;
      }
      i++;
      continue;
    }

    if (quote == '"') {
      if (c == '"') {
        quote = 0;
        i++;
        continue;
      }

      if (c == '\\' && i + 1 < line.size() &&
          string("$`\"\\").find(line[i + 1]) != string::npos) {
        cur.text += line[i + 1];
        i += 2;
        continue;
      }

      if (c != '$' && c != '`') {
        cur.text += c;
        i++;
        continue;
      }
    } else {
      if (c == '\'') {
        start_tok(false);
        quote = c;
        i++;
        continue;
      }

      if (c == '"') {
        start_tok(false);
        quote = c;
        i++;
        continue;
      }

      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        end_tok();
        i++;
        continue;
      }

      if (c == '>' && i + 1 < line.size() && line[i + 1] == '>') {
        end_tok();
        Tok op;
        op.text = ">>";
        op.op = true;
        toks.push_back(op);
        i += 2;
        continue;
      }

      if (c == '|' || c == '<' || c == '>') {
        end_tok();
        Tok op;
        op.text = string(1, c);
        op.op = true;
        toks.push_back(op);
        i++;
        continue;
      }

      if (c == '\\' && i + 1 < line.size()) {
        start_tok(false);
        cur.text += line[i + 1];
        i += 2;
        continue;
      }

      if (c != '$' && c != '`') {
        start_tok(true);
        cur.text += c;
        if (c == '*' || c == '?' || c == '[') {
          cur.glob = true;
        }
        i++;
        continue;
      }
    }

    if (c == '$') {
      bool quoted = (quote == '"');

      if (i + 1 < line.size() && line[i + 1] == '(') {
        size_t end = find_paren_end(line, i + 1);
        if (end == string::npos) {
          error = "terminator: syntax error: unclosed $(...)";
          return false;
        }
        add_expansion(capture_output(line.substr(i + 2, end - (i + 2)), tab), quoted);
        i = end + 1;
        continue;
      }

      if (i + 1 < line.size() && line[i + 1] == '{') {
        size_t end = line.find('}', i + 2);
        if (end == string::npos) {
          error = "terminator: syntax error: unclosed ${...}";
          return false;
        }
        add_expansion(get_var(line.substr(i + 2, end - (i + 2)), tab), quoted);
        i = end + 1;
        continue;
      }

      if (i + 1 < line.size() &&
          (isalpha((unsigned char)line[i + 1]) || line[i + 1] == '_')) {
        size_t j = i + 1;
        while (j < line.size() &&
               (isalnum((unsigned char)line[j]) || line[j] == '_')) {
          j++;
        }
        add_expansion(get_var(line.substr(i + 1, j - (i + 1)), tab), quoted);
        i = j;
        continue;
      }

      if (i + 1 < line.size() && (line[i + 1] == '?' || line[i + 1] == '$')) {
        add_expansion(get_var(string(1, line[i + 1]), tab), quoted);
        i += 2;
        continue;
      }

      if (i + 1 < line.size() && isdigit((unsigned char)line[i + 1])) {
        i += 2;
        continue;
      }

      start_tok(true);
      cur.text += '$';
      i++;
      continue;
    }

    if (c == '`') {
      bool quoted = (quote == '"');
      size_t j = i + 1, end = string::npos;

      while (j < line.size()) {
        if (line[j] == '\\' && j + 1 < line.size()) {
          j += 2;
        } else if (line[j] == '`') {
          end = j;
          break;
        } else {
          j++;
        }
      }

      if (end == string::npos) {
        error = "terminator: syntax error: unclosed backquote";
        return false;
      }

      add_expansion(capture_output(line.substr(i + 1, end - (i + 1)), tab), quoted);
      i = end + 1;
      continue;
    }

    cur.text += c;
    i++;
  }

  if (quote != 0) {
    error = "terminator: syntax error: unclosed quote";
    return false;
  }

  end_tok();

  auto expand_tilde = [&](const string &s, bool ok) {
    const char *home = getenv("HOME");
    if (!ok || !home) {
      return s;
    }
    if (s == "~") {
      return string(home);
    }
    if (s.rfind("~/", 0) == 0) {
      return string(home) + s.substr(1);
    }
    return s;
  };

  Command curcmd;

  for (size_t k = 0; k < toks.size(); k++) {
    Tok &t = toks[k];

    if (t.op && t.text == "|") {
      if (curcmd.args.empty() && curcmd.in_file.empty() && curcmd.out_file.empty()) {
        error = "terminator: syntax error near `|'";
        return false;
      }
      cmds.push_back(curcmd);
      curcmd = Command();
      continue;
    }

    if (t.op && (t.text == "<" || t.text == ">" || t.text == ">>")) {
      if (k + 1 >= toks.size() || toks[k + 1].op) {
        error = "terminator: syntax error near redirection";
        return false;
      }

      string file = expand_tilde(toks[k + 1].text, toks[k + 1].tilde);
      if (t.text == "<") {
        curcmd.in_file = file;
      } else {
        curcmd.out_file = file;
        curcmd.out_append = (t.text == ">>");
      }
      k++;
      continue;
    }

    string arg = expand_tilde(t.text, t.tilde);

    if (t.glob) {
      glob_t g;
      if (glob(arg.c_str(), 0, nullptr, &g) == 0 && g.gl_pathc > 0) {
        for (size_t m = 0; m < g.gl_pathc; m++) {
          curcmd.args.push_back(g.gl_pathv[m]);
        }
        globfree(&g);
      } else {
        curcmd.args.push_back(arg);
      }
    } else {
      curcmd.args.push_back(arg);
    }
  }

  if (!curcmd.args.empty() || !curcmd.in_file.empty() || !curcmd.out_file.empty()) {
    cmds.push_back(curcmd);
  }

  if (cmds.empty()) {
    error = "terminator: syntax error: empty command";
    return false;
  }

  for (auto &cmd : cmds) {
    if (cmd.args.empty() && cmd.in_file.empty() && cmd.out_file.empty()) {
      error = "terminator: syntax error near `|'";
      return false;
    }
  }

  return true;
}
