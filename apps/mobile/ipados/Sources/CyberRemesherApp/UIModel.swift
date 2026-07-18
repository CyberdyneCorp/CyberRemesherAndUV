// UNVERIFIED: iPadOS shell — decoders for the shared data-driven UI model
// (task 8.6). Pure Swift + Foundation (no UIKit), so it type-checks with
// `swift build`. Mirrors `../../shared/stages.json` and
// `../../shared/toolbar.default.json`; the Android shell decodes the same files.

import Foundation

/// One editing stage in the stage switcher (Retopology → UV → Baking).
public struct Stage: Codable, Identifiable, Equatable, Sendable {
    public let id: String
    public let title: String
    /// SF Symbol name for the iPadOS tab; the Android shell reads `androidIcon`.
    public let systemImage: String
    public let summary: String
}

/// One action (tool) that can appear in the toolbar or the Action Gallery.
public struct ActionDescriptor: Codable, Identifiable, Equatable, Sendable {
    public let id: String
    public let title: String
    public let systemImage: String
    /// Stage ids in which the action is meaningful; empty = all stages.
    public let stages: [String]
    /// Opaque engine chord/command id the shell injects (routing is the
    /// engine's concern — the shell never interprets it).
    public let command: String
}

/// Decoded `stages.json`.
public struct StageModel: Codable, Sendable {
    public let schemaVersion: Int
    public let stages: [Stage]
}

/// Decoded `toolbar.default.json`.
public struct ToolbarModel: Codable, Sendable {
    public let schemaVersion: Int
    public let actions: [ActionDescriptor]
    /// Ordered action ids shown in the toolbar by default.
    public let defaultToolbar: [String]
}

/// Loads the shared UI model from the app bundle, falling back to a minimal
/// built-in so the shell still renders if the resources are missing.
public enum UIModelLoader {
    public static func loadStages(bundle: Bundle = .main) -> StageModel {
        decode("stages", as: StageModel.self, bundle: bundle)
            ?? StageModel(schemaVersion: 1, stages: [
                Stage(id: "retopology", title: "Retopology",
                      systemImage: "scribble.variable", summary: ""),
                Stage(id: "uv", title: "UV", systemImage: "map", summary: ""),
                Stage(id: "baking", title: "Baking", systemImage: "paintbrush",
                      summary: ""),
            ])
    }

    public static func loadToolbar(bundle: Bundle = .main) -> ToolbarModel {
        decode("toolbar.default", as: ToolbarModel.self, bundle: bundle)
            ?? ToolbarModel(schemaVersion: 1, actions: [], defaultToolbar: [])
    }

    private static func decode<T: Decodable>(
        _ resource: String, as _: T.Type, bundle: Bundle
    ) -> T? {
        guard let url = bundle.url(forResource: resource, withExtension: "json"),
              let data = try? Data(contentsOf: url)
        else { return nil }
        return try? JSONDecoder().decode(T.self, from: data)
    }
}
