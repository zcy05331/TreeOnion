"""Add the RC public row layout to the already-staged upstream sources."""
from pathlib import Path
import sys


def replace_once(text: str, before: str, after: str) -> str:
    if text.count(before) != 1:
        raise RuntimeError(f"SPIRIT RC patch expected one anchor: {before[:80]!r}")
    return text.replace(before, after, 1)


def main() -> None:
    root = Path(sys.argv[1]) / "SPIRIT" / "pir"
    path = root / "types.h"
    text = path.read_text()
    start = text.index("    struct BatchParams")
    prefix, batch = text[:start], text[start:]
    batch = replace_once(batch, "        bool benchmark_randomize_hint_matrix = false;", "        bool benchmark_randomize_hint_matrix = false;\n        // Adapter RC layout: one strided response polynomial per bucket.\n        size_t rc_response_poly_degree = 0;")
    batch = replace_once(batch, "        std::vector<BatchBucketItemLayout> item_layouts;", "        size_t byte_row_stride = 1;\n        std::vector<BatchBucketItemLayout> item_layouts;")
    path.write_text(prefix + batch)

    path = root / "batch_protocol.cpp"
    text = path.read_text()
    text = replace_once(text,
        "BatchBucketLayout build_bucket_layout(size_t item_slots, size_t record_size_bytes)",
        "BatchBucketLayout build_bucket_layout(size_t item_slots, size_t record_size_bytes, size_t rc_poly_degree)")
    text = replace_once(text, "            layout.record_size_bytes = record_size_bytes;", """            layout.record_size_bytes = record_size_bytes;

            if (rc_poly_degree != 0)
            {
                if (rc_poly_degree % record_size_bytes != 0)
                {
                    throw std::invalid_argument("RC polynomial degree must be divisible by record size");
                }
                const size_t rho = rc_poly_degree / record_size_bytes;
                if (rho == 0 || (rho & (rho - 1)) != 0)
                {
                    throw std::invalid_argument("RC record residue capacity must be a power of two");
                }
                layout.items_per_column = rho;
                layout.rows = rc_poly_degree;
                layout.cols = std::max(size_t{1}, (item_slots + rho - 1) / rho);
                layout.byte_row_stride = rho;
                layout.item_layouts.resize(item_slots);
                for (size_t slot = 0; slot < item_slots; ++slot)
                {
                    layout.item_layouts[slot] = {slot / rho, slot % rho};
                }
                return layout;
            }""")
    text = replace_once(text,
        "build_bucket_layout(item_slots, params.record_size_bytes);",
        "build_bucket_layout(item_slots, params.record_size_bytes, params.rc_response_poly_degree);")
    text = replace_once(text,
        "                    size_t row_index = item_layout.start_row_index + byte_index;",
        "                    size_t row_index = item_layout.start_row_index + byte_index * layout.byte_row_stride;")
    text = replace_once(text,
        "const size_t row_index = item_layout.start_row_index + byte_index;",
        "const size_t row_index = item_layout.start_row_index + byte_index * bucket_layout.byte_row_stride;")
    path.write_text(text)


if __name__ == "__main__":
    main()
