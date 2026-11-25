#include "settings.hpp"

namespace ma::host {

Settings& settings() {
    static Settings s;
    return s;
}

}
