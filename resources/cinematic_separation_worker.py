import argparse
import inspect
import json
import os
import re
import shutil
import sys
import tempfile
from pathlib import Path


def find_roformer_stems(outputs, output_dir, voice_name, me_name, primary_stem, secondary_stem):
    """Resolve Audio Separator's returned paths and map either naming convention to our two stems."""
    output_dir = Path(output_dir)
    candidates = {}
    for raw_path in outputs or []:
        path = Path(raw_path)
        if not path.is_absolute():
            path = output_dir / path
        candidates[path.resolve()] = path
    for path in output_dir.glob("*.wav"):
        candidates[path.resolve()] = path

    primary = str(primary_stem or "Vocals").casefold()
    secondary = str(secondary_stem or "Instrumental").casefold()
    by_stem = {"voice": None, "me": None}
    for path in candidates.values():
        name = path.stem.casefold()
        if voice_name.casefold() in name or "vocal" in name or "voice" in name or (primary != "other" and primary in name):
            by_stem["voice"] = path
        elif me_name.casefold() in name or "instrumental" in name or "music" in name or "other" in name or secondary in name:
            by_stem["me"] = path
    return by_stem, sorted(str(path) for path in candidates.values())


def ensure_small_roformer_config(config_path):
    """Repair the small Unwa checkpoint's model-section expansion factor."""
    path = Path(config_path)
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()

    def indentation(line):
        return len(line) - len(line.lstrip(" \t"))

    model_match = next(
        (
            (index, indentation(line))
            for index, line in enumerate(lines)
            if re.match(r"^[ \t]*model[ \t]*:[ \t]*(?:#.*)?$", line)
        ),
        None,
    )
    if model_match is None:
        raise RuntimeError("La configuration du modèle léger ne contient pas de section model.")
    model_start, model_indent = model_match
    model_end = len(lines)
    for index in range(model_start + 1, len(lines)):
        line = lines[index]
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if indentation(line) <= model_indent and re.match(r"^[^#\s][^:]*\s*:", line):
            model_end = index
            break

    dim_index = None
    child_indent = None
    for index in range(model_start + 1, model_end):
        match = re.match(r"^([ \t]+)dim:[ \t]*384[ \t]*(?:#.*)?$", lines[index])
        if match and len(match.group(1)) > model_indent:
            dim_index = index
            child_indent = len(match.group(1))
            break
    if dim_index is None:
        raise RuntimeError("La configuration du modèle léger ne correspond pas au checkpoint attendu (model.dim=384).")

    factor_indices = []
    for index in range(model_start + 1, model_end):
        match = re.match(r"^([ \t]*)mlp_expansion_factor[ \t]*:.*$", lines[index])
        if match and len(match.group(1)) == child_indent:
            factor_indices.append(index)

    if factor_indices and all(
        re.match(r"^[ \t]*mlp_expansion_factor[ \t]*:[ \t]*1[ \t]*(?:#.*)?$", lines[index])
        for index in factor_indices
    ) and len(factor_indices) == 1:
        return

    if factor_indices:
        first = factor_indices[0]
        lines[first] = " " * child_indent + "mlp_expansion_factor: 1"
        for index in reversed(factor_indices[1:]):
            del lines[index]
    else:
        lines.insert(dim_index + 1, " " * child_indent + "mlp_expansion_factor: 1")

    repaired = "\n".join(lines) + "\n"
    verify_model = re.search(
        r"(?ms)^[ \t]*model[ \t]*:[ \t]*(?:#.*)?\n(?P<body>(?:(?:[ \t]+.*|[ \t]*#.*|[ \t]*)\n)*)(?=^[^ \t#\n][^:\n]*[ \t]*:|\Z)",
        repaired,
    )
    if not verify_model or not re.search(
        r"(?m)^[ \t]+dim:[ \t]*384[ \t]*(?:#.*)?$",
        verify_model.group("body"),
    ) or not re.search(
        r"(?m)^[ \t]+mlp_expansion_factor:[ \t]*1[ \t]*(?:#.*)?$",
        verify_model.group("body"),
    ):
        raise RuntimeError("Impossible de réparer la configuration model du modèle léger.")

    temporary_path = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", newline="\n",
            dir=str(path.parent), prefix=path.name + ".repair-", delete=False
        ) as stream:
            temporary_path = stream.name
            stream.write(repaired)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_path, path)
    finally:
        if temporary_path and os.path.exists(temporary_path):
            os.unlink(temporary_path)
def patch_librosa_duration_api():
    """Keep compatibility with older librosa-duration callers in separator backends."""
    import librosa

    parameters = inspect.signature(librosa.get_duration).parameters
    if "filename" not in parameters and "path" in parameters:
        original_get_duration = librosa.get_duration

        def get_duration_compat(*args, filename=None, **kwargs):
            if filename is not None:
                kwargs.setdefault("path", filename)
            return original_get_duration(*args, **kwargs)

        librosa.get_duration = get_duration_compat


def patch_mps_adaptive_avg_pool1d(torch):
    """Use CPU only for adaptive-pool shapes not supported by the MPS backend."""
    functional = torch.nn.functional
    original = functional.adaptive_avg_pool1d
    if getattr(original, "_rythmo_mps_compatible", False):
        return
    fallback_reported = False

    def adaptive_avg_pool1d_compatible(input, output_size):
        nonlocal fallback_reported
        if input.device.type != "mps":
            return original(input, output_size)
        target_size = output_size[0] if isinstance(output_size, (tuple, list)) else output_size
        target_size = int(target_size)
        if target_size > 0 and input.shape[-1] % target_size != 0:
            # MPS currently rejects these non-divisible adaptive-pooling shapes.
            # Keep all surrounding inference on Metal and offload only this op.
            if not fallback_reported:
                print("Compatibilité Metal : pooling adaptatif ponctuel exécuté sur CPU.", flush=True)
                fallback_reported = True
            return original(input.cpu(), output_size).to(device=input.device, dtype=input.dtype)
        return original(input, output_size)

    adaptive_avg_pool1d_compatible._rythmo_mps_compatible = True
    functional.adaptive_avg_pool1d = adaptive_avg_pool1d_compatible


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--model-dir", required=True)
    args = parser.parse_args()

    import numpy as np
    import soundfile as sf
    import torch
    patch_mps_adaptive_avg_pool1d(torch)
    model_files = {
        "Mel-RoFormer-Small-Unwa": (
            "melband_roformer_small_v1.ckpt",
            "config_melbandroformer_small.yaml",
            "Mel-Band RoFormer Small by unwa",
        ),
        "BS-RoFormer-Viperx-1297": (
            "model_bs_roformer_ep_317_sdr_12.9755.ckpt",
            "model_bs_roformer_ep_317_sdr_12.9755.yaml",
            "BS-RoFormer ViperX 1297",
        ),
        "Mel-RoFormer-Unwa-v1e-plus": (
            "inst_v1e_plus.ckpt",
            "config_melbandroformer_inst.yaml",
            "Mel-Band RoFormer Instrumental v1e+ by unwa",
        ),
    }
    if args.model not in model_files:
        raise RuntimeError(f"Modèle cinématique inconnu : {args.model}")

    if torch.cuda.is_available():
        device = torch.device("cuda:0")
    elif getattr(torch.backends, "mps", None) is not None and torch.backends.mps.is_available():
        device = torch.device("mps")
    else:
        device = torch.device("cpu")
    print(f"Calcul RoFormer · appareil détecté : {device}.", flush=True)
    torch.set_num_threads(max(1, min(8, os.cpu_count() or 1)))
    torch.backends.cudnn.benchmark = device.type == "cuda"
    patch_librosa_duration_api()
    from audio_separator.separator import Separator

    checkpoint_name, config_name, friendly_name = model_files[args.model]
    model_directory = Path(args.model_dir).resolve()
    checkpoint_path = model_directory / checkpoint_name
    config_path = model_directory / config_name
    if not checkpoint_path.is_file() or checkpoint_path.stat().st_size < 1024:
        raise RuntimeError(f"Poids RoFormer manquants ou incomplets : {checkpoint_path}")
    if not config_path.is_file():
        raise RuntimeError(f"Configuration RoFormer introuvable : {config_path}")
    if args.model == "Mel-RoFormer-Small-Unwa":
        ensure_small_roformer_config(config_path)
    roformer_separator = Separator(
        output_dir=str(Path(args.manifest).parent / "roformer-stems"),
        model_file_dir=str(model_directory),
        output_format="WAV",
        sample_rate=44100,
        use_soundfile=True,
    )

    # Register the checksum-verified local checkpoint and config with the
    # separator API. This avoids its online model catalogue and extra downloads.
    def load_local_model_files(model_filename):
        if model_filename != checkpoint_name:
            raise RuntimeError(f"Modèle local inattendu : {model_filename}")
        return checkpoint_name, "MDXC", friendly_name, str(checkpoint_path), str(config_path)

    roformer_separator.download_model_files = load_local_model_files
    roformer_separator.load_model(model_filename=checkpoint_name)
    print(f"Moteur RoFormer prêt · {getattr(roformer_separator, 'torch_device', device)}.", flush=True)

    with open(args.manifest, "r", encoding="utf-8") as stream:
        jobs = json.load(stream)
    if not jobs:
        raise RuntimeError("Aucun fichier audio à traiter.")

    for index, job in enumerate(jobs, 1):
        source = Path(job["input"])
        voice_target = Path(job["voice_output"])
        me_target = Path(job["me_output"])
        audio, rate = sf.read(str(source), dtype="float32", always_2d=True)
        required_rate = 44100
        if rate != required_rate:
            raise RuntimeError(f"Le moteur attend {required_rate} Hz ; {source.name} est à {rate} Hz.")
        if audio.shape[1] == 0 or audio.shape[0] == 0:
            raise RuntimeError(f"La piste audio est vide : {source.name}.")
        job["channels"] = int(audio.shape[1])

        voice_channels = []
        me_channels = []
        print(f"Séparation {index}/{len(jobs)} · {source.name} · {audio.shape[1]} canaux", flush=True)
        if args.model in model_files:
            work_root = Path(args.manifest).parent / "roformer-work" / f"job-{index}"
            if work_root.exists():
                shutil.rmtree(work_root)
            work_root.mkdir(parents=True, exist_ok=True)
            try:
                channel_groups = [list(range(2))] if audio.shape[1] == 2 else [[channel] for channel in range(audio.shape[1])]
                for group_index, channels in enumerate(channel_groups):
                    input_path = work_root / f"input-{group_index}.wav"
                    if len(channels) == 2:
                        model_input = audio[:, channels]
                    else:
                        mono = audio[:, channels[0]]
                        model_input = np.column_stack((mono, mono))
                    sf.write(str(input_path), model_input, rate, subtype="PCM_24", format="WAV")
                    output_dir = work_root / f"output-{group_index}"
                    output_dir.mkdir(parents=True, exist_ok=True)
                    voice_name = f"voice-{index}-{group_index}"
                    me_name = f"me-{index}-{group_index}"
                    roformer_separator.output_dir = str(output_dir)
                    if roformer_separator.model_instance is not None:
                        roformer_separator.model_instance.output_dir = str(output_dir)
                    # The public API propagates single-file errors while also handling
                    # device precision and cleanup consistently across supported releases.
                    outputs = roformer_separator.separate(str(input_path))
                    model_instance = roformer_separator.model_instance
                    by_stem, found_files = find_roformer_stems(
                        outputs,
                        output_dir,
                        voice_name,
                        me_name,
                        getattr(model_instance, "primary_stem_name", "Vocals"),
                        getattr(model_instance, "secondary_stem_name", "Instrumental"),
                    )
                    if by_stem["voice"] is None or by_stem["me"] is None:
                        raise RuntimeError(
                            f"BS-RoFormer n’a pas retourné ses deux stems pour {source.name}. "
                            f"Fichiers retournés : {outputs!r} ; fichiers trouvés : {found_files!r}."
                        )
                    voice, voice_rate = sf.read(str(by_stem["voice"]), dtype="float32", always_2d=True)
                    me, me_rate = sf.read(str(by_stem["me"]), dtype="float32", always_2d=True)
                    if voice_rate != required_rate or me_rate != required_rate or voice.shape[0] != audio.shape[0] or me.shape != voice.shape:
                        raise RuntimeError(f"BS-RoFormer a retourné des stems de format ou durée inattendus pour {source.name}.")
                    if len(channels) == 2:
                        voice_channels = [voice[:, channel] for channel in range(2)]
                        me_channels = [me[:, channel] for channel in range(2)]
                    else:
                        voice_channels.append(voice[:, 0])
                        me_channels.append(me[:, 0])
            finally:
                shutil.rmtree(work_root, ignore_errors=True)
        voice_audio = np.stack(voice_channels, axis=1)
        me_audio = np.stack(me_channels, axis=1)
        voice_target.parent.mkdir(parents=True, exist_ok=True)
        me_target.parent.mkdir(parents=True, exist_ok=True)
        sf.write(str(voice_target), voice_audio, rate, subtype="PCM_24", format="WAV")
        sf.write(str(me_target), me_audio, rate, subtype="PCM_24", format="WAV")
        for target in (voice_target, me_target):
            if not target.is_file() or target.stat().st_size < 1024:
                raise RuntimeError(f"Le fichier traité est absent ou incomplet : {target.name}.")
        print(f"Terminé {index}/{len(jobs)} · Voix + M&E · {audio.shape[1]} canaux", flush=True)

    with open(args.manifest, "w", encoding="utf-8") as stream:
        json.dump(jobs, stream, ensure_ascii=False)
    print("RythmoCinematicJobComplete", flush=True)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"ERREUR: {error}", file=sys.stderr, flush=True)
        raise
