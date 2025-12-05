#include "utils.hpp"

std::string NormalizePinyin(const std::string& p) {
    std::string s = p;
    // Map zh->z, ch->c, sh->s
    if (s.size() >= 2) {
        if (s.substr(0, 2) == "zh") s = "z" + s.substr(2);
        else if (s.substr(0, 2) == "ch") s = "c" + s.substr(2);
        else if (s.substr(0, 2) == "sh") s = "s" + s.substr(2);
    }
    // Map ng->n (ang->an, eng->en, ing->in, ong->on)
    if (s.size() >= 2 && s.substr(s.size()-2) == "ng") {
        s = s.substr(0, s.size()-1);
    }
    return s;
}
