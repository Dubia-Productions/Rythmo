# Rythmo

**Le logiciel de création de bande rythmo et de séparation vocale par excellence.**

Rythmo est un logiciel de postproduction disponible sur Windows, macOS et Linux. Les projets, vidéos et traitements audio restent sur l’appareil.

## Fonctionnalités

- Créer, ouvrir et enregistrer des projets DetX.
- Lier une vidéo, créer des rôles et synchroniser les répliques sur quatre pistes.
- Lire la vidéo en fenêtre ou en plein écran et exporter la bande à 60 i/s, de 720p à 4K.
- Choisir un encodeur disponible sur la machine et activer ou désactiver les effets visuels d’export.
- Séparer localement une scène en deux fichiers : voix et musique/effets. Les trois profils utilisent des modèles RoFormer à 44,1 kHz ; le profil élevé utilise Mel-Band RoFormer Instrumental v1e+ by unwa.
- Reprendre un projet récent.
- Recevoir une notification facultative quand une version plus récente est disponible. Rythmo télécharge le paquet correspondant à la plateforme, demande confirmation avant le redémarrage, installe la mise à jour puis relance l’application. Aucune vérification réseau n’est faite quand l’appareil est hors ligne.

Le moteur local et le modèle RoFormer choisi sont préparés à la première utilisation. Les modèles installés peuvent être supprimés depuis l’application. FFmpeg sert au décodage et à l’export vidéo.

## Démarrage rapide

1. Ouvrir **Bande Rythmo**, puis créer un projet ou ouvrir un fichier DetX.
2. Choisir la vidéo source et ajouter rôles et répliques sur la bande.
3. Cliquer sur **Exporter** pour choisir la résolution, l’encodeur et les effets.
4. Ouvrir **Séparation de voix**, sélectionner un profil, ajouter les médias et lancer le traitement.

## Raccourcis et gestes

| Action | Touche ou geste |
| --- | --- |
| Lecture / pause | `Espace` |
| Plein écran / quitter | `F11` / `Échap` |
| Ajouter une réplique à la position de lecture | `Entrée` sur la bande |
| Modifier la réplique sélectionnée | `F2` ou double-clic |
| Supprimer la sélection | `Suppr` |
| Scinder la réplique sélectionnée | `Ctrl+K` (`⌘+K` sur macOS) |
| Avancer ou reculer d’une image | `Alt+←` / `Alt+→` ou `Maj+←` / `Maj+→` |
| Zoomer sur la bande | `+` / `-` ou molette |
| Naviguer dans la bande | Défilement à deux doigts du trackpad |
| Annuler / rétablir | `Ctrl+Z` / `Ctrl+Maj+Z` ou `⌘+Z` / `⌘+Maj+Z` |
| Nouveau, ouvrir, enregistrer | Raccourcis affichés dans le menu **Fichier** |

Un clic droit sur la bande ajoute une réplique à l’emplacement choisi. Sur une réplique, il ouvre les commandes de modification et de suppression. Dans le champ de texte d’une réplique, `Entrée` valide la saisie sans créer de saut de ligne. `Ctrl+K` scinde une réplique ; ce raccourci ne marque pas un changement de plan.

## Compilation

Les scripts détectent et installent les dépendances usuelles, compilent en mode Release et lancent les tests :

- Windows x64 : `build-windows-x64.bat` ; Windows ARM64 : `build-windows-arm64.bat`.
- macOS Intel : `build-macos-intel.sh` ; Apple Silicon : `build-macos-silicon.sh`.
- Linux x64 : `build-linux-x64.sh` ; Linux ARM64 : `build-linux-arm64.sh`.
- Détection automatique : `build.bat` sous Windows ou `sh build.sh` sous macOS/Linux.

Une compilation manuelle nécessite CMake 3.25+, Qt 6.5+ (Widgets, Multimedia, MultimediaWidgets et Network), Expat et un compilateur C++17.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Le workflow GitHub Actions construit les six cibles x64 et ARM lors de chaque commit sur la branche `Release`. Les releases versionnées publient un paquet d’installation et une archive portable pour chaque cible prise en charge.

---

Certaines fonctionnalités, notamment le système de mise à jour, ont été peaufinées avec l’aide d’outils d’intelligence artificielle.
