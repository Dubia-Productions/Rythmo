(() => {
  const FALLBACK_VERSION = "0.4.8";
  const REPO = "Dubia-Productions/Rythmo";
  const BASE_RELEASE_DOWNLOAD = `https://github.com/${REPO}/releases/latest/download`;

  const ASSETS = {
    "windows-x64-setup": {
      file: "Rythmo-windows-x64-setup.exe",
      label: "Installateur Windows (x64)",
      short: "Windows x64 (.exe)"
    },
    "windows-x64-portable": {
      file: "Rythmo-windows-x64-portable.zip",
      label: "Archive portable Windows (x64)",
      short: "Windows x64 (.zip)"
    },
    "windows-arm64-setup": {
      file: "Rythmo-windows-arm64-setup.exe",
      label: "Installateur Windows (ARM64)",
      short: "Windows ARM64 (.exe)"
    },
    "windows-arm64-portable": {
      file: "Rythmo-windows-arm64-portable.zip",
      label: "Archive portable Windows (ARM64)",
      short: "Windows ARM64 (.zip)"
    },
    "macos-arm64": {
      file: "Rythmo-macos-arm64.zip",
      label: "macOS Apple Silicon (M1–M4)",
      short: "macOS Apple Silicon (.zip)"
    },
    "macos-x64": {
      file: "Rythmo-macos-x64.zip",
      label: "macOS Intel (x64)",
      short: "macOS Intel (.zip)"
    },
    "linux-x64": {
      file: "Rythmo-linux-x64-portable.tar.gz",
      label: "Linux x64 (portable)",
      short: "Linux x64 (.tar.gz)"
    },
    "linux-arm64": {
      file: "Rythmo-linux-arm64-portable.tar.gz",
      label: "Linux ARM64 (portable)",
      short: "Linux ARM64 (.tar.gz)"
    }
  };

  async function detectPlatform() {
    const ua = navigator.userAgent || "";
    const platform = (navigator.platform || "").toLowerCase();
    let os = "windows";
    let arch = "x64";

    if (/mac/i.test(platform) || /mac os x|macintosh/i.test(ua)) {
      os = "macos";
      arch = "arm64";
      try {
        const gl = document.createElement("canvas").getContext("webgl");
        const dbg = gl && gl.getExtension("WEBGL_debug_renderer_info");
        const renderer = dbg ? gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL) : "";
        if (/intel/i.test(renderer) && !/apple/i.test(renderer)) {
          arch = "x64";
        }
      } catch (_) {}
    } else if (/win/i.test(platform) || /windows/i.test(ua)) {
      os = "windows";
      arch = /arm64|aarch64/i.test(ua) ? "arm64" : "x64";
    } else if (/linux/i.test(platform) || /linux/i.test(ua)) {
      os = "linux";
      arch = /aarch64|arm64/i.test(ua) ? "arm64" : "x64";
    }

    if (navigator.userAgentData && navigator.userAgentData.getHighEntropyValues) {
      try {
        const hints = await navigator.userAgentData.getHighEntropyValues(["architecture", "platform"]);
        if (hints.platform) {
          const p = hints.platform.toLowerCase();
          if (p.includes("mac")) os = "macos";
          else if (p.includes("win")) os = "windows";
          else if (p.includes("linux")) os = "linux";
        }
        if (hints.architecture) {
          arch = hints.architecture.toLowerCase().includes("arm") ? "arm64" : "x64";
        }
      } catch (_) {}
    }

    let key = "windows-x64-setup";
    if (os === "macos") key = arch === "arm64" ? "macos-arm64" : "macos-x64";
    else if (os === "windows") key = arch === "arm64" ? "windows-arm64-setup" : "windows-x64-setup";
    else if (os === "linux") key = arch === "arm64" ? "linux-arm64" : "linux-x64";

    return { os, arch, key };
  }

  function formatSize(bytes) {
    if (!bytes || typeof bytes !== "number") return "";
    const mb = bytes / (1024 * 1024);
    return `${mb.toFixed(1).replace(".", ",")} Mo`;
  }

  async function hydrateReleaseData(detected) {
    const versionEls = document.querySelectorAll("[data-rythmo-version]");
    const assetLinks = document.querySelectorAll("[data-asset]");

    const setFallbackLinks = () => {
      assetLinks.forEach((el) => {
        const key = el.getAttribute("data-asset");
        const spec = ASSETS[key];
        if (spec) el.setAttribute("href", `${BASE_RELEASE_DOWNLOAD}/${spec.file}`);
      });
    };

    setFallbackLinks();

    const recBtns = document.querySelectorAll("[data-recommended-btn]");
    const recTitle = document.querySelector("[data-recommended-title]");
    const recMeta = document.querySelector("[data-recommended-meta]");
    const recCard = document.querySelector(`[data-platform-card="${detected.os}"]`);

    if (recCard) {
      recCard.classList.add("is-detected");
      const badge = recCard.querySelector("[data-detected-badge]");
      if (badge) badge.textContent = "Détecté";
    }

    const spec = ASSETS[detected.key];
    if (spec) {
      recBtns.forEach((btn) => {
        btn.textContent = `Télécharger pour ${spec.short}`;
        btn.setAttribute("href", `${BASE_RELEASE_DOWNLOAD}/${spec.file}`);
      });
    }
    if (recTitle && spec) {
      recTitle.textContent = spec.label;
    }

    try {
      const response = await fetch(`https://api.github.com/repos/${REPO}/releases/latest`, {
        headers: { Accept: "application/vnd.github+json" }
      });
      if (!response.ok) return;
      const release = await response.json();
      const tag = release.tag_name ? release.tag_name.replace(/^v/, "") : FALLBACK_VERSION;

      versionEls.forEach((el) => {
        el.textContent = `v${tag}`;
      });

      const byName = new Map();
      if (Array.isArray(release.assets)) {
        release.assets.forEach((a) => byName.set(a.name, a));
      }

      assetLinks.forEach((el) => {
        const key = el.getAttribute("data-asset");
        const s = ASSETS[key];
        if (!s) return;
        const asset = byName.get(s.file);
        if (asset) {
          el.setAttribute("href", asset.browser_download_url);
          const sizeTarget = el.closest("[data-download-row]")?.querySelector("[data-asset-size]");
          if (sizeTarget && asset.size) {
            sizeTarget.textContent = formatSize(asset.size);
          }
        }
      });

      if (spec && byName.has(spec.file)) {
        const matched = byName.get(spec.file);
        recBtns.forEach((btn) => btn.setAttribute("href", matched.browser_download_url));
        if (recMeta) {
          const sizeText = matched.size ? ` · ${formatSize(matched.size)}` : "";
          recMeta.textContent = `Version ${tag}${sizeText} · FFmpeg inclus`;
        }
      }
    } catch (_) {
      // Silencieux : les liens directs /releases/latest/download/ restent fonctionnels.
    }
  }

  function initStudioPreview() {
    const tabs = document.querySelectorAll("[data-studio-tab]");
    const panels = document.querySelectorAll("[data-studio-panel]");
    tabs.forEach((tab) => {
      tab.addEventListener("click", () => {
        const target = tab.getAttribute("data-studio-tab");
        tabs.forEach((t) => t.classList.toggle("is-active", t === tab));
        panels.forEach((p) => p.classList.toggle("is-active", p.getAttribute("data-studio-panel") === target));
      });
    });

    const playBtn = document.querySelector("[data-preview-play]");
    const cursor = document.querySelector("[data-preview-cursor]");
    const timecode = document.querySelector("[data-preview-timecode]");
    if (!playBtn || !cursor || !timecode) return;

    let playing = false;
    let progress = 36;
    let timer = null;

    const formatTc = (pct) => {
      const totalFrames = Math.round((pct / 100) * 600);
      const sec = Math.floor(totalFrames / 24);
      const frm = totalFrames % 24;
      const ss = String(14 + sec).padStart(2, "0");
      const ff = String(frm).padStart(2, "0");
      return `00:02:${ss}:${ff}`;
    };

    const stop = () => {
      playing = false;
      playBtn.textContent = "Lecture";
      if (timer) {
        clearInterval(timer);
        timer = null;
      }
    };

    playBtn.addEventListener("click", () => {
      if (playing) {
        stop();
        return;
      }
      playing = true;
      playBtn.textContent = "Pause";
      timer = setInterval(() => {
        progress += 1.4;
        if (progress > 88) progress = 14;
        cursor.style.left = `${progress}%`;
        timecode.textContent = formatTc(progress);
      }, 120);
    });
  }

  document.addEventListener("DOMContentLoaded", async () => {
    initStudioPreview();
    const detected = await detectPlatform();
    hydrateReleaseData(detected);
  });
})();
