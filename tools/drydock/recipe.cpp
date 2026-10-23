#include "recipe.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace drydock {

namespace {

    std::string Trim(const std::string &s) {
        size_t a = 0, b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
        return s.substr(a, b - a);
    }

    // Removes a trailing comment (a '#' outside quotes).
    std::string StripComment(const std::string &s) {
        bool quoted = false;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '"') quoted = !quoted;
            else if (s[i] == '#' && !quoted) return s.substr(0, i);
        }
        return s;
    }

    struct Value {
        enum Kind { String, Integer, Boolean } kind;
        std::string str;
        uint64_t num = 0;
        bool flag = false;
    };

    bool ParseValue(const std::string &text, Value &v) {
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
            v.kind = Value::String;
            v.str = text.substr(1, text.size() - 2);
            return true;
        }
        if (text == "true" || text == "false") {
            v.kind = Value::Boolean;
            v.flag = text == "true";
            return true;
        }
        char *end = nullptr;
        std::string digits;
        for (char c : text)
            if (c != '_') digits += c;
        v.num = strtoull(digits.c_str(), &end, 0);
        if (end && *end == '\0' && !digits.empty()) {
            v.kind = Value::Integer;
            return true;
        }
        return false;
    }

} // namespace

bool LoadRecipe(const std::string &path, Recipe &recipe, std::string &error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot read recipe " + path;
        return false;
    }
    recipe = Recipe{};
    RecipeModule *module = nullptr;
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        lineNo++;
        line = Trim(StripComment(line));
        if (line.empty()) continue;
        if (line == "[[module]]") {
            recipe.modules.emplace_back();
            module = &recipe.modules.back();
            continue;
        }
        const size_t eq = line.find('=');
        Value v;
        if (eq == std::string::npos || !ParseValue(Trim(line.substr(eq + 1)), v)) {
            error = path + ":" + std::to_string(lineNo) + ": cannot parse '" + line + "'";
            return false;
        }
        const std::string key = Trim(line.substr(0, eq));
        auto str = [&](std::string &dst) {
            if (v.kind != Value::String) return false;
            dst = v.str;
            return true;
        };
        bool ok = true;
        if (module) {
            if (key == "file") ok = str(module->file);
            else if (key == "sha256") ok = str(module->sha256);
            else if (key == "base") {
                ok = v.kind == Value::Integer;
                module->base = (uint32_t)v.num;
            } else if (key == "trace") {
                ok = v.kind == Value::Boolean;
                module->useTrace = v.flag;
            } else ok = false;
        } else {
            if (key == "ship") ok = str(recipe.ship);
            else if (key == "title") ok = str(recipe.title);
            else if (key == "region") ok = str(recipe.region);
            else if (key == "product") ok = str(recipe.product);
            else if (key == "trace") ok = str(recipe.tracePath);
            else ok = false;
        }
        if (!ok) {
            error = path + ":" + std::to_string(lineNo) + ": unexpected '" + key + "'";
            return false;
        }
    }
    if (recipe.product.empty() || recipe.modules.empty()) {
        error = path + ": a recipe needs a product number and at least one module";
        return false;
    }
    // trace path relative to the recipe
    if (!recipe.tracePath.empty() && recipe.tracePath[0] != '/' &&
        !(recipe.tracePath.size() > 1 && recipe.tracePath[1] == ':')) {
        const size_t slash = path.find_last_of("/\\");
        if (slash != std::string::npos) recipe.tracePath = path.substr(0, slash + 1) + recipe.tracePath;
    }
    return true;
}

} // namespace drydock
