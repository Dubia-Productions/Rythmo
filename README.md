# Rythmo

**Le logiciel de création de bande rythmo et de séparation vocale par excellence.**

Rythmo est un logiciel de postproduction local disponible sur Windows, macOS et Linux. Vos projets, médias et traitements restent sur votre appareil.

## Fonctionnalités

- Créer, ouvrir et enregistrer des projets DetX.
- Synchroniser les répliques, rôles et quatre pistes avec une vidéo.
- Lire la vidéo dans l’éditeur ou en plein écran.
- Exporter la bande rythmo en 720p à 4K, jusqu’à 60 images/s, avec les encodeurs disponibles sur l’appareil.
- Activer ou désactiver les effets visuels à l’export.
- Séparer une scène localement en deux fichiers : voix et musique/effets, avec trois profils RoFormer.
- Reprendre un projet récent.
- Vérifier les mises à jour en ligne, uniquement si une connexion est détectée. La mise à jour reste facultative, le paquet est vérifié avant installation et l’application redémarre automatiquement.

Les modèles de séparation se téléchargent à la première utilisation du profil. FFmpeg est inclus dans les paquets de l’application.

## Démarrage rapide

1. Ouvrir **Bande Rythmo** et créer un projet ou ouvrir un fichier DetX.
2. Choisir la vidéo source, puis ajouter rôles et répliques.
3. Cliquer sur **Exporter** pour choisir la résolution, l’encodeur et les effets.
4. Ouvrir **Séparation de voix**, choisir un profil et ajouter des fichiers.

## Raccourcis et gestes

| Action | Touche ou geste |
| --- | --- |
| Lecture / pause | Espace |
| Plein écran / quitter | F11 / Échap |
| Ajouter une réplique à la position de lecture | Entrée sur la bande |
| Modifier la réplique sélectionnée | F2 ou double-clic |
| Supprimer la sélection | Suppr |
| Scinder une réplique | Ctrl+K (⌘+K sur macOS) |
| Avancer ou reculer d’une image | Alt+flèches ou Maj+flèches |
| Zoomer sur la bande | + / - ou molette |
| Naviguer sur la bande | Défilement à deux doigts du trackpad |
| Annuler / rétablir | Ctrl+Z / Ctrl+Maj+Z ou ⌘+Z / ⌘+Maj+Z |

Un clic droit sur la bande ajoute une réplique à l’emplacement choisi. Sur une réplique, il ouvre les commandes de modification et de suppression. Dans le champ de texte, Entrée valide la saisie. Ctrl+K scinde une réplique ; il ne marque pas un changement de plan.

## Téléchargements et compatibilité

Chaque release fournit :

- **Windows x64 et ARM64** : installateur et archive portable.
- **macOS Intel et Apple Silicon** : un seul ZIP contenant directement Rythmo.app.
- **Linux x64 et ARM64** : archive portable .tar.gz.

La version macOS utilise Qt 6.8.3, compilé avec une cible minimale **macOS 12 (Monterey)**. Les Mac Intel sont compilés nativement, mais macOS Sierra (10.12) et les versions 10.13 à 11 ne sont pas compatibles avec cette base Qt. Les prendre en charge exigerait un portage séparé vers une ancienne version de Qt et des dépendances IA compatibles.

## Compilation

Le workflow GitHub Actions compile les six cibles en parallèle sur les runners x64 et ARM64, met en cache Qt, les outils et les runtimes Python, et publie les paquets lorsqu’un tag vX.Y.Z est poussé. Les exécutions de branche plus anciennes sont annulées lorsqu’un nouveau commit arrive.

Compilation locale : CMake 3.25+, Qt 6.5+ (Widgets, Multimedia, MultimediaWidgets et Network), Expat et un compilateur C++17.

~~~sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
~~~


## v0.4.1

- Choix des profils de séparation vocale simplifié : faible, moyen ou élevé.
- Import local d’un checkpoint RoFormer et de sa configuration YAML pour les utilisateurs avancés.
- Icône Rythmo appliquée à l’application Windows, macOS et Linux.
