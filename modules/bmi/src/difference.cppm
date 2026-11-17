// mcpp.bmi.difference — what two BMIs of one module differ in.
//
// The vocabulary the cutoff and every format reader share. `Same`: equal for
// an importer (bytes, or bytes but for what the format's reader masks).
// `LocationsOnly`: the recorded source locations moved and nothing else; an
// importer of the previous BMI reads the previous lines. `Changed`: anything
// an importer can observe.

export module mcpp.bmi.difference;

export namespace mcpp::bmi {

enum class Difference { Same, LocationsOnly, Changed };

} // namespace mcpp::bmi
