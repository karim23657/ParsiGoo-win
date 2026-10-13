// User pronunciation dictionary — Persian word -> romanised phonemes.
//
// Lets a user correct a word the G2P gets wrong (or pronounce a name/term their
// own way) without touching the model:
//
//   بُردارسازی    bordArAzi        <- "word<TAB>phonemes", UTF-8, one per line
//
// Entries live in <base>/dicts/*.dict (user.dict is the read/write one used by
// the GUI, the web UI and the /api/dict API) and are merged into the G2P: a
// word the dictionary knows is replaced by its phonemes, everything else still
// goes through the model. Lookup is accent/case/punctuation insensitive, so
// «بُردارسازی» and «بردارسازی» hit the same entry.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "g2p.h"

namespace tts {

class Dictionary {
public:
    struct Entry {
        std::string word;      // as the user typed it
        std::string phonemes;  // romanised phonemes ("bordArAzi")
    };

    void clear() { map_.clear(); }
    size_t size() const { return map_.size(); }
    bool empty() const { return map_.empty(); }

    // Merge `path`; a missing file is not an error (returns 0).
    size_t load_file(const std::string& path);
    // Merge every *.dict in `dir` (sorted, so the result is deterministic).
    size_t load_dir(const std::string& dir);
    // Merge inline entries, one per line: "word<TAB>phonemes" ("#" = comment).
    size_t load_text(const std::string& text);

    // Insert or replace. Returns false when the entry is empty/invalid.
    bool set(const std::string& word, const std::string& phonemes);
    bool erase(const std::string& word);
    bool has(const std::string& word) const { return map_.count(key(word)) != 0; }
    // Phonemes for `word`, or "" when the dictionary does not know it.
    std::string lookup(const std::string& word) const;
    // True when any whitespace-delimited word of `text` is in the dictionary.
    bool has_match(const std::string& text) const;
    // True when at least one phoneme value is multi-word (changes how
    // phonemise_phrase aligns words — see dict.cpp).
    bool value_has_spaces() const;

    // Entries ordered by normalised key (stable output for saving/UI).
    std::vector<Entry> entries() const;

    // Write the dictionary back (creates parent directories). Returns false on
    // an I/O error.
    bool save_file(const std::string& path) const;

    // Normalised lookup key: strips whitespace/punctuation/diacritics/ZWNJ,
    // folds Arabic letter forms to Persian, lowercases ASCII.
    static std::string key(const std::string& word);

private:
    std::map<std::string, Entry> map_;
};

// Whitespace split of a UTF-8 string (ASCII whitespace only, as elsewhere).
std::vector<std::string> dict_split_words(const std::string& s);

// G2P + dictionary for a single phrase.
//
// Without a dictionary (or without a hit) this is exactly G2P::phonemise, so
// the default pipeline is unchanged. With a hit it first tries one G2P call for
// the whole phrase and replaces the known words positionally (keeps the phrase
// context); when the word counts do not line up it falls back to phonemising
// the runs of unknown words together and splicing the dictionary words in.
// `keep_ezafe` also controls whether the "1" ezafe marker is kept in the
// dictionary value.
std::string phonemise_phrase(G2P& g2p, const Dictionary* dict, const std::string& text, bool keep_ezafe);

}  // namespace tts
