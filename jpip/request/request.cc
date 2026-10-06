#include "request.h"

#include <algorithm>
#include <cstdint>
#include <climits>
#include <cstring>
#include <vector>

using namespace std;

namespace jpip {

    namespace {

        struct QueryParameter {
            string name, value;
        };

        using Query = vector<QueryParameter>;

        bool IsRoutingParameter(const char *begin, const char *end) {
            size_t length = end - begin;
            return (length == 4 && memcmp(begin, "cnew", 4) == 0) ||
                   (length == 3 && memcmp(begin, "cid", 3) == 0) ||
                   (length == 6 && memcmp(begin, "cclose", 6) == 0);
        }

        template<typename Function>
        bool ForEachParameter(const char *begin, const char *end,
                              Function function) {
            while (begin < end) {
                const char *separator = static_cast<const char *>(
                        memchr(begin, '&', end - begin));
                const char *parameter_end = separator ? separator : end;
                const char *equals = static_cast<const char *>(
                        memchr(begin, '=', parameter_end - begin));
                const char *name_end = equals ? equals : parameter_end;
                const char *value_begin = equals ? equals + 1 : parameter_end;
                if (!function(begin, name_end, value_begin, parameter_end))
                    return false;
                if (!separator)
                    break;
                begin = separator + 1;
            }
            return true;
        }

        Query ParseQuery(const char *begin, const char *end) {
            Query query;
            ForEachParameter(begin, end,
                             [&query](const char *name_begin,
                                      const char *name_end,
                                      const char *value_begin,
                                      const char *value_end) {
                                 query.push_back({string(name_begin, name_end),
                                                  string(value_begin, value_end)});
                                 return true;
                             });
            return query;
        }

        Query ParseTargetQuery(const string &target) {
            size_t question = target.find('?');
            if (question == string::npos)
                return Query();
            return ParseQuery(target.data() + question + 1,
                              target.data() + target.size());
        }

        const string *FindParameter(const Query &query, const char *name) {
            for (Query::const_reverse_iterator i = query.rbegin(); i != query.rend(); ++i)
                if (i->name == name)
                    return &i->value;
            return NULL;
        }

        bool ParseUnsignedInteger(const char **position, uint64_t maximum, uint64_t *value) {
            const char *current = *position;
            if (*current < '0' || *current > '9')
                return false;

            uint64_t number = 0;
            do {
                uint64_t digit = *current - '0';
                if (digit > maximum || number > (maximum - digit) / 10)
                    return false;
                number = number * 10 + digit;
                ++current;
            } while (*current >= '0' && *current <= '9');

            *position = current;
            *value = number;
            return true;
        }

        bool IsScheme(const string &uri, size_t length) {
            if (length == 0 ||
                !((uri[0] >= 'A' && uri[0] <= 'Z') ||
                  (uri[0] >= 'a' && uri[0] <= 'z')))
                return false;
            for (size_t i = 1; i < length; ++i) {
                char c = uri[i];
                if (!((c >= 'A' && c <= 'Z') ||
                      (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '+' || c == '-' ||
                      c == '.'))
                    return false;
            }
            return true;
        }

        string GetObject(const string &uri, size_t end) {
            if (uri.empty() || uri[0] == '/')
                return uri.substr(0, end);

            size_t colon = uri.find(':');
            if (colon == string::npos || colon >= end || !IsScheme(uri, colon))
                return uri.substr(0, end);

            size_t path = colon + 1;
            if (path + 1 < end && uri[path] == '/' && uri[path + 1] == '/') {
                path = uri.find('/', path + 2);
                if (path == string::npos || path >= end)
                    return "/";
            }
            return uri.substr(path, end - path);
        }

        void SetError(string *error_message, const string &message) {
            if (error_message != NULL && error_message->empty())
                *error_message = message;
        }

        bool ParseInteger(const char **position, int *value) {
            const char *current = *position;
            bool negative = *current == '-';
            if (negative)
                ++current;

            uint64_t maximum = negative ? static_cast<uint64_t>(INT_MAX) + 1 : INT_MAX;
            uint64_t number;
            if (!ParseUnsignedInteger(&current, maximum, &number))
                return false;
            *position = current;
            if (negative)
                *value = number == maximum ? INT_MIN : -static_cast<int>(number);
            else
                *value = static_cast<int>(number);
            return true;
        }

        bool ParsePair(const string &text, int *x, int *y, string *suffix = NULL) {
            const char *position = text.c_str();
            if (!ParseInteger(&position, x) || *position++ != ',' ||
                !ParseInteger(&position, y))
                return false;
            if (*position == '\0') {
                if (suffix)
                    suffix->clear();
                return true;
            }
            if (!suffix || *position++ != ',')
                return false;
            *suffix = position;
            return !suffix->empty();
        }

        bool ParseRange(const char **position, uint64_t *first, uint64_t *last,
                        uint64_t maximum = UINT64_MAX) {
            if (!ParseUnsignedInteger(position, maximum, first))
                return false;
            *last = *first;
            if (**position == '-') {
                ++*position;
                if (**position >= '0' && **position <= '9') {
                    if (!ParseUnsignedInteger(position, maximum, last) ||
                        *last < *first)
                        return false;
                } else {
                    *last = UINT64_MAX;
                }
            }
            return true;
        }

        bool ParseTransports(const string &value, bool *accepts_http) {
            *accepts_http = false;
            size_t begin = 0;
            for (;;) {
                size_t end = value.find(',', begin);
                if (end == string::npos)
                    end = value.size();
                if (end == begin)
                    return false;
                for (size_t i = begin; i < end; ++i) {
                    char c = value[i];
                    if (!((c >= 'A' && c <= 'Z') ||
                          (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') ||
                          c == '.' || c == '_' || c == '-'))
                        return false;
                }
                if (value.compare(begin, end - begin, "http") == 0)
                    *accepts_http = true;
                if (end == value.size())
                    return true;
                begin = end + 1;
            }
        }

        bool HexDigit(char character, unsigned char *value) {
            if (character >= '0' && character <= '9')
                *value = character - '0';
            else if (character >= 'a' && character <= 'f')
                *value = character - 'a' + 10;
            else if (character >= 'A' && character <= 'F')
                *value = character - 'A' + 10;
            else
                return false;
            return true;
        }

        bool Decode(const string &text, string *decoded) {
            decoded->clear();
            decoded->reserve(text.size());
            for (size_t i = 0; i < text.size(); ++i) {
                if (text[i] != '%') {
                    decoded->push_back(text[i]);
                    continue;
                }
                unsigned char high, low;
                if (i + 2 >= text.size() ||
                    !HexDigit(text[i + 1], &high) || !HexDigit(text[i + 2], &low))
                    return false;
                char character = static_cast<char>((high << 4) | low);
                if (character == '\0')
                    return false;
                decoded->push_back(character);
                i += 2;
            }
            return true;
        }

        bool ParseContext(const string &value, uint64_t *first, uint64_t *last) {
            string decoded;
            if (!Decode(value, &decoded) || decoded.compare(0, 5, "jpxl<") != 0 ||
                decoded.size() < 7 || decoded.back() != '>')
                return false;

            decoded.pop_back();
            const char *position = decoded.c_str() + 5;
            return ParseRange(&position, first, last) && *position == '\0';
        }

        bool ParseModel(const string &value, vector<Request::ModelUpdate> *model,
                        string *error_message) {
            string text;
            if (!Decode(value, &text) || text.empty())
                return false;

            model->clear();
            int minimum_codestream = 0;
            int maximum_codestream = 0;
            bool has_codestream_qualifier = false;
            bool descriptor_found = false;
            const char *position = text.c_str();

            while (*position != '\0') {
                if (*position == ',') {
                    if (*++position == '\0')
                        return false;
                    continue;
                }
                if (*position == '[') {
                    ++position;
                    uint64_t first, last;
                    if (!ParseRange(&position, &first, &last,
                                    ResponseRequest::MAX_CODESTREAM_INDEX))
                        return false;
                    minimum_codestream = static_cast<int>(first);
                    maximum_codestream = last == UINT64_MAX ? INT_MAX : static_cast<int>(last);
                    if (*position++ != ']')
                        return false;
                    has_codestream_qualifier = true;
                    continue;
                }
                if (*position == '-') {
                    SetError(error_message, "Subtractive bin-descriptors are not supported for model updating");
                    return false;
                }

                char type = *position++;
                int id = 0;
                if (type == 'H' && *position == 'm') {
                    type = 'h';
                    ++position;
                } else if (!ParseInteger(&position, &id) || id < 0) {
                    return false;
                }

                int amount = INT_MAX;
                if (*position == ':') {
                    ++position;
                    if (*position == 'L') {
                        SetError(error_message, "Number of layers can not be used for model updating");
                        return false;
                    }
                    if (!ParseInteger(&position, &amount) || amount < 0)
                        return false;
                }

                Request::ModelUpdate update;
                update.has_codestream_qualifier = has_codestream_qualifier;
                update.first_codestream = minimum_codestream;
                update.last_codestream = maximum_codestream;
                update.id = id;
                update.amount = amount;
                if (type == 'M') {
                    update.bin_class = DataBinClass::META_DATA;
                } else if (type == 'h') {
                    update.bin_class = DataBinClass::MAIN_HEADER;
                } else if (type == 'H') {
                    update.bin_class = DataBinClass::TILE_HEADER;
                } else if (type == 'P') {
                    update.bin_class = DataBinClass::PRECINCT;
                } else {
                    SetError(error_message, "The bin-descriptor '" + string(1, type) +
                             "' is not supported for model updating");
                    return false;
                }
                model->push_back(update);
                descriptor_found = true;
            }
            return descriptor_found;
        }

    }

    bool HasRoutingParameter(const string &target) {
        size_t question = target.find('?');
        if (question == string::npos)
            return false;

        const char *begin = target.data() + question + 1;
        const char *end = target.data() + target.size();
        bool visited_all = ForEachParameter(
                begin, end,
                [](const char *name_begin, const char *name_end,
                   const char *, const char *) {
                    return !IsRoutingParameter(name_begin, name_end);
                });
        return !visited_all;
    }

    bool Request::ParseStream(const string &value) {
        const char *position = value.c_str();
        while (*position != '\0') {
            uint64_t range_first, last;
            if (!ParseRange(&position, &range_first, &last))
                return false;

            uint64_t step = 1;
            if (*position == ':') {
                ++position;
                if (!ParseUnsignedInteger(&position, UINT64_MAX, &step) || step == 0)
                    return false;
            }

            AddStream(range_first, last, step);
            if (*position == '\0')
                return true;
            if (*position++ != ',' || *position == '\0')
                return false;
        }
        return false;
    }

    bool Request::ParseTarget(const string &uri, string *error_message) {
        if (error_message != NULL)
            error_message->clear();
        size_t question = uri.find('?');
        if (!Decode(GetObject(uri, question), &object)) {
            SetError(error_message, "Invalid escaped target path");
            return false;
        }

        if (question == string::npos)
            return true;

        bool valid = true;
        Query query = ParseTargetQuery(uri);
        const string *tid = FindParameter(query, "tid");
        bool accept_model = tid == NULL || *tid == "0";
        for (const QueryParameter &parameter : query) {
            const string &name = parameter.name;
            const string &value = parameter.value;
            int x, y;

            if (name == "target") {
                routing.target = true;
                if (!Decode(value, &target)) {
                    valid = false;
                    SetError(error_message, "Invalid escaped target parameter");
                }
            } else if (name == "cid") {
                routing.cid = true;
            } else if (name == "cnew") {
                if (ParseTransports(value, &accepts_http))
                    routing.cnew = true;
                else {
                    valid = false;
                    SetError(error_message, "Invalid JPIP cnew parameter");
                }
            } else if (name == "cclose") {
                if (value == "*") {
                    valid = false;
                    SetError(error_message,
                             "Closing every JPIP channel is not supported");
                } else {
                    routing.cclose = true;
                }
            } else if (name == "metareq") {
                has_metareq = true;
            } else if (name == "fsiz") {
                string round;
                bool parsed = ParsePair(value, &x, &y, &round);
                RoundDirection direction = ROUNDDOWN;
                if (parsed) {
                    if (round.empty() || round == "round-down")
                        direction = ROUNDDOWN;
                    else if (round == "round-up")
                        direction = ROUNDUP;
                    else if (round == "closest")
                        direction = CLOSEST;
                    else
                        parsed = false;
                }
                if (parsed) {
                    resolution_size = Size(x, y);
                    round_direction = direction;
                    has.fsiz = true;
                } else {
                    valid = false;
                    SetError(error_message, "Invalid JPIP fsiz parameter");
                }
            } else if (name == "roff") {
                if (ParsePair(value, &x, &y)) {
                    woi_position = Point(x, y);
                    has.roff = true;
                } else {
                    valid = false;
                    SetError(error_message, "Invalid JPIP roff parameter");
                }
            } else if (name == "rsiz") {
                if (ParsePair(value, &x, &y)) {
                    woi_size = Size(x, y);
                    has.rsiz = true;
                } else {
                    valid = false;
                    SetError(error_message, "Invalid JPIP rsiz parameter");
                }
            } else if (name == "len") {
                const char *position = value.c_str();
                if (ParseInteger(&position, &x) && *position == '\0' && x >= 0) {
                    length_response = x;
                    has.len = true;
                } else {
                    valid = false;
                    SetError(error_message, "Invalid JPIP len parameter");
                }
            } else if (name == "layers") {
                const char *position = value.c_str();
                if (!ParseUnsignedInteger(&position, UINT64_MAX, &layers) ||
                    *position != '\0') {
                    valid = false;
                    SetError(error_message, "Invalid JPIP layers parameter");
                }
            } else if (name == "tid") {
                routing.tid = true;
            } else if (name == "handled") {
                routing.handled = true;
            } else if (name == "stream") {
                if (!ParseStream(value)) {
                    valid = false;
                    SetError(error_message, "Invalid JPIP stream parameter");
                }
            } else if (name == "model" && accept_model) {
                if (ParseModel(value, &model, error_message)) {
                    has.model = true;
                } else {
                    valid = false;
                    SetError(error_message,
                             "Invalid or unsupported JPIP model parameter");
                }
            } else if (name == "context") {
                uint64_t first, last;
                if (ParseContext(value, &first, &last)) {
                    AddContext(first, last);
                } else {
                    valid = false;
                    SetError(error_message, "Invalid JPIP context parameter");
                }
            }
        }

        const string *route = FindParameter(query, routing.cclose ? "cclose" : "cid");
        if (route)
            channel = *route;
        if (routing.cnew && routing.cclose) {
            valid = false;
            SetError(error_message,
                     "JPIP cnew and cclose can not be combined");
        }
        if ((has.roff || has.rsiz) && !has.fsiz) {
            valid = false;
            SetError(error_message,
                     "JPIP fsiz is required with roff or rsiz");
        }
        return valid;
    }

}
