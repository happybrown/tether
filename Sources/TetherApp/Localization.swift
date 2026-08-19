import SwiftUI

/// 앱 표시 언어. rawValue 는 lproj 폴더명과 일치한다 — 새 언어를 추가하려면
/// case 하나와 Resources/<code>.lproj/Localizable.strings 만 추가하면 된다.
enum AppLanguage: String, CaseIterable, Identifiable {
    case system  = "system"
    case english = "en"
    case korean  = "ko"

    var id: String { rawValue }

    /// 선택 메뉴에 보여줄 이름. 언어 이름은 그 언어 자체로 표기해
    /// 현재 언어를 몰라도 자기 언어를 찾을 수 있게 한다.
    var label: String {
        switch self {
        case .system:  return L("language.system")
        case .english: return "English"
        case .korean:  return "한국어"
        }
    }
}

/// 문자열 현지화 관리자. 선택 언어의 lproj 번들에서 키를 찾아 반환한다.
/// @Published 언어 변경이 관찰 중인 뷰를 다시 그리므로 재시작 없이 즉시 반영된다.
final class L10n: ObservableObject {
    static let shared = L10n()
    private static let defaultsKey = "appLanguage"

    @Published var language: AppLanguage {
        didSet {
            UserDefaults.standard.set(language.rawValue, forKey: Self.defaultsKey)
            bundle = Self.resolveBundle(for: language)
        }
    }

    /// 현재 언어의 문자열 번들. 시스템 기본이면 main 번들(OS 언어 설정을 따름).
    private var bundle: Bundle

    private init() {
        let saved = UserDefaults.standard.string(forKey: Self.defaultsKey) ?? ""
        let lang = AppLanguage(rawValue: saved) ?? .system
        language = lang
        bundle = Self.resolveBundle(for: lang)
    }

    /// 키에 해당하는 현지화 문자열. 키가 없으면 키 자체를 반환한다(누락 진단용).
    func string(_ key: String) -> String {
        bundle.localizedString(forKey: key, value: nil, table: nil)
    }

    private static func resolveBundle(for language: AppLanguage) -> Bundle {
        guard language != .system,
              let path = Bundle.main.path(forResource: language.rawValue, ofType: "lproj"),
              let bundle = Bundle(path: path) else { return .main }
        return bundle
    }
}

/// 현지화 문자열 조회 단축 함수. 뷰가 L10n.shared 를 관찰하고 있어야
/// 언어 변경 시 다시 그려진다.
func L(_ key: String) -> String {
    L10n.shared.string(key)
}

/// 포맷 인자가 있는 현지화 문자열 (예: L("log.count_format", count)).
func L(_ key: String, _ args: CVarArg...) -> String {
    String(format: L10n.shared.string(key), arguments: args)
}
