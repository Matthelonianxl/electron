// Copyright (c) 2014 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/renderer/api/atom_api_spell_check_client.h"

#include <map>
#include <set>
#include <unordered_set>
#include <utility>
#include <vector>

#include "atom/common/native_mate_converters/string16_converter.h"
#include "base/logging.h"
#include "base/numerics/safe_conversions.h"
#include "base/threading/thread_task_runner_handle.h"
#include "components/spellcheck/renderer/spellcheck_worditerator.h"
#include "native_mate/converter.h"
#include "native_mate/dictionary.h"
#include "native_mate/function_template.h"
#include "third_party/blink/public/web/web_text_checking_completion.h"
#include "third_party/blink/public/web/web_text_checking_result.h"
#include "third_party/icu/source/common/unicode/uscript.h"

namespace atom {

namespace api {

namespace {

bool HasWordCharacters(const base::string16& text, int index) {
  const base::char16* data = text.data();
  int length = text.length();
  while (index < length) {
    uint32_t code = 0;
    U16_NEXT(data, index, length, code);
    UErrorCode error = U_ZERO_ERROR;
    if (uscript_getScript(code, &error) != USCRIPT_COMMON)
      return true;
  }
  return false;
}

struct Word {
  blink::WebTextCheckingResult result;
  base::string16 text;
  std::vector<base::string16> contraction_words;
};

}  // namespace

class SpellCheckClient::SpellcheckRequest {
 public:
  SpellcheckRequest(
      const base::string16& text,
      std::unique_ptr<blink::WebTextCheckingCompletion> completion)
      : text_(text), completion_(std::move(completion)) {}
  ~SpellcheckRequest() {}

  const base::string16& text() const { return text_; }
  blink::WebTextCheckingCompletion* completion() { return completion_.get(); }
  std::vector<Word>& wordlist() { return word_list_; }

 private:
  base::string16 text_;          // Text to be checked in this task.
  std::vector<Word> word_list_;  // List of Words found in text
  // The interface to send the misspelled ranges to WebKit.
  std::unique_ptr<blink::WebTextCheckingCompletion> completion_;

  DISALLOW_COPY_AND_ASSIGN(SpellcheckRequest);
};

SpellCheckClient::SpellCheckClient(const std::string& language,
                                   v8::Isolate* isolate,
                                   v8::Local<v8::Object> provider)
    : pending_request_param_(nullptr),
      isolate_(isolate),
      context_(isolate, isolate->GetCurrentContext()),
      provider_(isolate, provider) {
  DCHECK(!context_.IsEmpty());

  character_attributes_.SetDefaultLanguage(language);

  // Persistent the method.
  mate::Dictionary dict(isolate, provider);
  dict.Get("spellCheck", &spell_check_);
}

SpellCheckClient::~SpellCheckClient() {
  context_.Reset();
}

void SpellCheckClient::RequestCheckingOfText(
    const blink::WebString& textToCheck,
    std::unique_ptr<blink::WebTextCheckingCompletion> completionCallback) {
  base::string16 text(textToCheck.Utf16());
  // Ignore invalid requests.
  if (text.empty() || !HasWordCharacters(text, 0)) {
    completionCallback->DidCancelCheckingText();
    return;
  }

  // Clean up the previous request before starting a new request.
  if (pending_request_param_) {
    pending_request_param_->completion()->DidCancelCheckingText();
  }

  pending_request_param_.reset(
      new SpellcheckRequest(text, std::move(completionCallback)));

  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(&SpellCheckClient::SpellCheckText, AsWeakPtr()));
}

bool SpellCheckClient::IsSpellCheckingEnabled() const {
  return true;
}

void SpellCheckClient::ShowSpellingUI(bool show) {}

bool SpellCheckClient::IsShowingSpellingUI() {
  return false;
}

void SpellCheckClient::UpdateSpellingUIWithMisspelledWord(
    const blink::WebString& word) {}

void SpellCheckClient::SpellCheckText() {
  const auto& text = pending_request_param_->text();
  if (text.empty() || spell_check_.IsEmpty()) {
    pending_request_param_->completion()->DidCancelCheckingText();
    pending_request_param_ = nullptr;
    return;
  }

  if (!text_iterator_.IsInitialized() &&
      !text_iterator_.Initialize(&character_attributes_, true)) {
    // We failed to initialize text_iterator_, return as spelled correctly.
    VLOG(1) << "Failed to initialize SpellcheckWordIterator";
    return;
  }

  if (!contraction_iterator_.IsInitialized() &&
      !contraction_iterator_.Initialize(&character_attributes_, false)) {
    // We failed to initialize the word iterator, return as spelled correctly.
    VLOG(1) << "Failed to initialize contraction_iterator_";
    return;
  }

  text_iterator_.SetText(text.c_str(), text.size());

  SpellCheckScope scope(*this);
  base::string16 word;
  size_t word_start;
  size_t word_length;
  std::set<base::string16> words;
  auto& word_list = pending_request_param_->wordlist();
  Word word_entry;
  for (;;) {  // Run until end of text
    const auto status =
        text_iterator_.GetNextWord(&word, &word_start, &word_length);
    if (status == SpellcheckWordIterator::IS_END_OF_TEXT)
      break;
    if (status == SpellcheckWordIterator::IS_SKIPPABLE)
      continue;

    word_entry.result.location = base::checked_cast<int>(word_start);
    word_entry.result.length = base::checked_cast<int>(word_length);
    word_entry.text = word;
    word_entry.contraction_words.clear();

    word_list.push_back(word_entry);
    words.insert(word);
    // If the given word is a concatenated word of two or more valid words
    // (e.g. "hello:hello"), we should treat it as a valid word.
    if (IsContraction(scope, word, &word_entry.contraction_words)) {
      for (const auto& w : word_entry.contraction_words) {
        words.insert(w);
      }
    }
  }

  // Send out all the words data to the spellchecker to check
  SpellCheckWords(scope, words);
}

void SpellCheckClient::OnSpellCheckDone(
    const std::vector<base::string16>& misspelled_words) {
  std::vector<blink::WebTextCheckingResult> results;
  std::unordered_set<base::string16> misspelled(misspelled_words.begin(),
                                                misspelled_words.end());

  auto& word_list = pending_request_param_->wordlist();

  for (const auto& word : word_list) {
    if (misspelled.find(word.text) != misspelled.end()) {
      // If this is a contraction, iterate through parts and accept the word
      // if none of them are misspelled
      if (!word.contraction_words.empty()) {
        auto all_correct = true;
        for (const auto& contraction_word : word.contraction_words) {
          if (misspelled.find(contraction_word) != misspelled.end()) {
            all_correct = false;
            break;
          }
        }
        if (all_correct)
          continue;
      }
      results.push_back(word.result);
    }
  }
  pending_request_param_->completion()->DidFinishCheckingText(results);
  pending_request_param_ = nullptr;
}

void SpellCheckClient::SpellCheckWords(const SpellCheckScope& scope,
                                       const std::set<base::string16>& words) {
  DCHECK(!scope.spell_check_.IsEmpty());

  v8::Local<v8::FunctionTemplate> templ = mate::CreateFunctionTemplate(
      isolate_,
      base::BindRepeating(&SpellCheckClient::OnSpellCheckDone, AsWeakPtr()));

  auto context = isolate_->GetCurrentContext();
  v8::Local<v8::Value> args[] = {mate::ConvertToV8(isolate_, words),
                                 templ->GetFunction(context).ToLocalChecked()};
  // Call javascript with the words and the callback function
  scope.spell_check_->Call(context, scope.provider_, 2, args).IsEmpty();
}

// Returns whether or not the given string is a contraction.
// This function is a fall-back when the SpellcheckWordIterator class
// returns a concatenated word which is not in the selected dictionary
// (e.g. "in'n'out") but each word is valid.
// Output variable contraction_words will contain individual
// words in the contraction.
bool SpellCheckClient::IsContraction(
    const SpellCheckScope& scope,
    const base::string16& contraction,
    std::vector<base::string16>* contraction_words) {
  DCHECK(contraction_iterator_.IsInitialized());

  contraction_iterator_.SetText(contraction.c_str(), contraction.length());

  base::string16 word;
  size_t word_start;
  size_t word_length;
  for (auto status =
           contraction_iterator_.GetNextWord(&word, &word_start, &word_length);
       status != SpellcheckWordIterator::IS_END_OF_TEXT;
       status = contraction_iterator_.GetNextWord(&word, &word_start,
                                                  &word_length)) {
    if (status == SpellcheckWordIterator::IS_SKIPPABLE)
      continue;

    contraction_words->push_back(word);
  }
  return contraction_words->size() > 1;
}

SpellCheckClient::SpellCheckScope::SpellCheckScope(
    const SpellCheckClient& client)
    : handle_scope_(client.isolate_),
      context_scope_(
          v8::Local<v8::Context>::New(client.isolate_, client.context_)),
      provider_(client.provider_.NewHandle()),
      spell_check_(client.spell_check_.NewHandle()) {}

SpellCheckClient::SpellCheckScope::~SpellCheckScope() = default;

}  // namespace api

}  // namespace atom
