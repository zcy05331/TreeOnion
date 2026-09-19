#!/usr/bin/env python3
import shutil
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: patch_spirit_core.py SPIRIT_DIR OUTPUT_DIR", file=sys.stderr)
        return 2

    spirit_dir = Path(sys.argv[1])
    output_dir = Path(sys.argv[2])
    source_dir = spirit_dir / "src" / "SPIRIT"
    if not (source_dir / "pir" / "batch_protocol.h").exists():
        print(f"invalid SPIRIT checkout: {spirit_dir}", file=sys.stderr)
        return 1

    target_dir = output_dir / "SPIRIT"
    if target_dir.exists():
        shutil.rmtree(target_dir)
    shutil.copytree(source_dir, target_dir)

    query_unpack = target_dir / "he" / "query_unpack.cpp"
    text = query_unpack.read_text(encoding="utf-8")
    text = text.replace(
        "std::chrono::system_clock::time_point start, end;",
        "std::chrono::high_resolution_clock::time_point start, end;",
    )
    text = text.replace(
        '        std::cout << "Cipher slots: " << cipher_slots << std::endl;\n',
        "",
    )
    query_unpack.write_text(text, encoding="utf-8")

    types_h = target_dir / "pir" / "types.h"
    text = types_h.read_text(encoding="utf-8")
    text = text.replace(
        """    struct BatchResponseMessage
    {
        std::vector<BatchBucketResponse> bucket_responses;
    };
} // namespace spirit::pir
""",
        """    struct BatchAnswerProfile
    {
        double secret_unpack_ms = 0.0;
        double matvec_ms = 0.0;
        double modswitch_ms = 0.0;
        double b_extract_ms = 0.0;
        double lwe_pack_ms = 0.0;
        double total_ms = 0.0;
    };

    struct BatchResponseMessage
    {
        std::vector<BatchBucketResponse> bucket_responses;
        BatchAnswerProfile profile;
    };
} // namespace spirit::pir
""",
    )
    types_h.write_text(text, encoding="utf-8")

    batch_protocol = target_dir / "pir" / "batch_protocol.cpp"
    text = batch_protocol.read_text(encoding="utf-8")
    text = text.replace(
        """        double total_ms = std::chrono::duration<double, std::milli>(clock::now() - total_start).count();
        // std::cout << std::fixed << std::setprecision(3);
""",
        """        double total_ms = std::chrono::duration<double, std::milli>(clock::now() - total_start).count();
        response.profile.secret_unpack_ms = secret_unpack_online_ms;
        response.profile.matvec_ms = matvec_ms;
        response.profile.modswitch_ms = modswitch_ms;
        response.profile.b_extract_ms = b_extract_ms;
        response.profile.lwe_pack_ms = lwe_pack_ms;
        response.profile.total_ms = total_ms;
        // std::cout << std::fixed << std::setprecision(3);
""",
    )
    batch_protocol.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
