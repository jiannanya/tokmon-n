#include "tokmon/builtin_lens.hpp"

#include "lenses/aya/aya_lens.hpp"
#include "lenses/calculator/calculator_lens.hpp"
#include "lenses/chora/chora_lens.hpp"
#include "lenses/cista/cista_lens.hpp"
#include "lenses/clotho/clotho_lens.hpp"
#include "lenses/cove/cove_lens.hpp"
#include "lenses/enso/enso_lens.hpp"
#include "lenses/fallen/fallen_lens.hpp"
#include "lenses/ignis/ignis_lens.hpp"
#include "lenses/iris/iris_lens.hpp"
#include "lenses/janus/janus_lens.hpp"
#include "lenses/lemon/lemon_lens.hpp"
#include "lenses/nota/nota_lens.hpp"
#include "lenses/rhea/rhea_lens.hpp"
#include "lenses/snow/snow_lens.hpp"
#include "lenses/styx/styx_lens.hpp"
#include "lenses/techor/techor_lens.hpp"
#include "lenses/termon/termon_lens.hpp"
#include "lenses/textus/textus_lens.hpp"
#include "lenses/tracket/tracket_lens.hpp"

namespace tokmon {

std::shared_ptr<ILens> make_builtin_lens(const std::string_view id) {
  if (id == "ignis") return std::make_shared<builtin::IgnisLens>();
  if (id == "lemon") return std::make_shared<builtin::LemonLens>();
  if (id == "iris") return std::make_shared<builtin::IrisLens>();
  if (id == "rhea") return std::make_shared<builtin::RheaLens>();
  if (id == "janus") return std::make_shared<builtin::JanusLens>();
  if (id == "clotho") return std::make_shared<builtin::ClothoLens>();
  if (id == "aya") return std::make_shared<builtin::AyaLens>();
  if (id == "textus") return std::make_shared<builtin::TextusLens>();
  if (id == "enso") return std::make_shared<builtin::EnsoLens>();
  if (id == "techor") return std::make_shared<builtin::TechorLens>();
  if (id == "fallen") return std::make_shared<builtin::FallenLens>();
  if (id == "cista") return std::make_shared<builtin::CistaLens>();
  if (id == "styx") return std::make_shared<builtin::StyxLens>();
  if (id == "chora") return std::make_shared<builtin::ChoraLens>();
  if (id == "tracket") return std::make_shared<builtin::TracketLens>();
  if (id == "nota") return std::make_shared<builtin::NotaLens>();
  if (id == "cove") return std::make_shared<builtin::CoveLens>();
  if (id == "snow") return std::make_shared<builtin::SnowLens>();
  if (id == "termon") return std::make_shared<builtin::TermonLens>();
  if (id == "calculator") return std::make_shared<builtin::CalculatorLens>();
  return {};
}

LensManifest builtin_lens_manifest(const std::string_view id) {
  const auto lens = make_builtin_lens(id);
  return lens ? lens->manifest() : LensManifest{};
}

std::vector<std::string> official_lens_order() {
  return {"ignis", "lemon", "iris", "rhea", "janus", "clotho", "aya",
          "textus", "enso", "techor", "styx", "fallen", "cista", "chora",
          "tracket", "nota", "cove", "snow", "termon"};
}

}  // namespace tokmon
