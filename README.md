<div align="center">

<img src="resources/RYTHMO_LOGO_PNG-ROUNDED.png" alt="Rythmo" width="110" />

# Rythmo

**Création de bande rythmo et séparation vocale, 100 % en local.**

[![Releases](https://img.shields.io/github/v/release/Dubia-Productions/Rythmo?label=Release&style=flat-square)](https://github.com/Dubia-Productions/Rythmo/releases)
![Plateformes](https://img.shields.io/badge/Plateformes-Windows%20%C2%B7%20macOS%20%C2%B7%20Linux-1f2937?style=flat-square)
![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square)
![Qt6](https://img.shields.io/badge/Qt-6.5%2B-41CD52?style=flat-square)

[Télécharger](https://github.com/Dubia-Productions/Rythmo/releases) · [Fonctionnalités](#fonctionnalités) · [Raccourcis](#raccourcis-et-gestes) · [Compilation](#compilation)

</div>

---

**Rythmo** est un logiciel de postproduction conçu pour le doublage et le traitement audio local. Vos projets, vidéos et traitements restent entièrement sur votre machine — disponible sur **Windows**, **macOS** et **Linux**.

## Fonctionnalités

- **Bande rythmo & DetX** — Créez, ouvrez et enregistrez des projets DetX ; synchronisez répliques, rôles et quatre pistes directement sur la vidéo.
- **Lecture & édition fluide** — Prévisualisez dans l'éditeur ou en plein écran, scindez les répliques, ajoutez des changements de plan et éditez le texte directement sur la bande.
- **Export vidéo jusqu'en 4K** — Exportez votre bande rythmo de 720p à 4K jusqu'à 60 ips avec les encodeurs matériels/logiciels disponibles sur l'appareil et les effets visuels activables à la carte (FFmpeg inclus).
- **Séparation de voix IA** — Isolez localement les voix de la musique et des effets (M&E) grâce à trois profils **RoFormer** (Rapide, Équilibré, Studio) ou à un modèle `.ckpt` personnalisé. Les modèles sont téléchargés uniquement à la première utilisation.
- **Mises à jour discrètes** — Vérification en ligne facultative si une connexion est détectée, avec vérification du paquet avant installation.

## Démarrage rapide

1. Ouvrez **Bande Rythmo** pour créer un projet ou charger un fichier `.detx`.
2. Associez la vidéo source, puis ajoutez vos rôles et répliques sur la bande.
3. Cliquez sur **Exporter** (<kbd>Ctrl</kbd>+<kbd>E</kbd> / <kbd>⌘</kbd>+<kbd>E</kbd>) pour choisir la résolution, l'encodeur et les effets.
4. Ouvrez **Séparation de voix**, sélectionnez un profil RoFormer et déposez vos fichiers audio/vidéo.

## Raccourcis et gestes

### Lecture & navigation

| Action | Raccourci / Geste |
| --- | --- |
| Lecture / Pause | <kbd>Espace</kbd> |
| Plein écran / Quitter | <kbd>F11</kbd> / <kbd>Échap</kbd> |
| Réplique ou plan précédent / suivant | <kbd>←</kbd> / <kbd>→</kbd> |
| Avancer ou reculer d'une image | <kbd>Alt</kbd>+<kbd>←</kbd> <kbd>→</kbd> ou <kbd>Maj</kbd>+<kbd>←</kbd> <kbd>→</kbd> |
| Zoomer / Dézoomer sur la bande | <kbd>+</kbd> / <kbd>-</kbd> ou molette |
| Naviguer dans le temps | Défilement à deux doigts (trackpad) ou clic sur la règle |

### Édition sur la bande

| Action | Raccourci / Geste |
| --- | --- |
| Insérer une réplique à la barre rouge | <kbd>Entrée</kbd> sur la bande (ou <kbd>Ctrl</kbd>+<kbd>Maj</kbd>+<kbd>N</kbd> / <kbd>⌘</kbd>+<kbd>Maj</kbd>+<kbd>N</kbd>) |
| Éditer la réplique sélectionnée | <kbd>F2</kbd>, double-clic ou clic sur la sélection |
| Passer à la réplique suivante / précédente (en édition) | <kbd>Tab</kbd> / <kbd>Maj</kbd>+<kbd>Tab</kbd> |
| Scinder une réplique | <kbd>Ctrl</kbd>+<kbd>K</kbd> / <kbd>⌘</kbd>+<kbd>K</kbd> |
| Ajouter un changement de plan | <kbd>Ctrl</kbd>+<kbd>M</kbd> / <kbd>⌘</kbd>+<kbd>M</kbd> |
| Supprimer la sélection | <kbd>Suppr</kbd> / <kbd>Retour arrière</kbd> |
| Annuler / Rétablir | <kbd>Ctrl</kbd>+<kbd>Z</kbd> · <kbd>Ctrl</kbd>+<kbd>Maj</kbd>+<kbd>Z</kbd> / <kbd>⌘</kbd>+<kbd>Z</kbd> · <kbd>⌘</kbd>+<kbd>Maj</kbd>+<kbd>Z</kbd> |

### Fichiers

| Action | Raccourci |
| --- | --- |
| Ouvrir / Enregistrer / Enregistrer sous | <kbd>Ctrl</kbd>+<kbd>O</kbd> · <kbd>S</kbd> · <kbd>Maj</kbd>+<kbd>S</kbd> (ou <kbd>⌘</kbd>) |
| Exporter la vidéo rythmo | <kbd>Ctrl</kbd>+<kbd>E</kbd> / <kbd>⌘</kbd>+<kbd>E</kbd> |

> **Astuce :** Un clic droit sur la bande ajoute une réplique à l'emplacement cliqué ; sur une réplique existante, il ouvre les options de modification et de suppression. Dans le champ de texte, <kbd>Entrée</kbd> valide la saisie.

## Téléchargement et compatibilité

Les paquets prêts à l'emploi sont disponibles dans les [GitHub Releases](https://github.com/Dubia-Productions/Rythmo/releases) :

| Plateforme | Architectures | Format | Notes |
| --- | --- | --- | --- |
| **Windows** | x64, ARM64 | Installateur `.exe` / Archive `.zip` | FFmpeg inclus |
| **macOS** | Apple Silicon, Intel | Archive `.zip` (`Rythmo.app`) | **macOS 12 (Monterey)** minimum |
| **Linux** | x64, ARM64 | Archive portable `.tar.gz` | FFmpeg inclus |

## Compilation

**Prérequis :** CMake 3.25+, Qt 6.5+ (`Widgets`, `Multimedia`, `MultimediaWidgets`, `Network`), Expat et un compilateur C++17.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```
