module;
#include <cstdio>

export module mcpp.shell;

import std;

export namespace mcpp::shell {

std::string_view script(std::string_view shell) {
    if (shell == "bash") return R"MCPP(# mcpp completion; source this file from ~/.bashrc.
_mcpp_complete() {
    local item mode prefix="" cur="${COMP_WORDS[COMP_CWORD]}"
    local -a words=() reply=()
    local i
    # Bash splits '=' and ':' by default; keep them inside mcpp arguments.
    for ((i=1; i<=COMP_CWORD; i++)); do
        if [[ ${COMP_WORDS[i]} == '=' || ${COMP_WORDS[i]} == ':' ]]; then
            if ((${#words[@]})); then
                words[${#words[@]}-1]+="${COMP_WORDS[i]}"
            fi
        elif ((i>1)) && [[ ${COMP_WORDS[i-1]} == '=' || ${COMP_WORDS[i-1]} == ':' ]]; then
            words[${#words[@]}-1]+="${COMP_WORDS[i]}"
        else
            words+=("${COMP_WORDS[i]}")
        fi
    done
    while IFS= read -r item; do reply+=("$item"); done < <(command "${COMP_WORDS[0]}" __complete "${words[@]}" 2>/dev/null)
    mode=${reply[0]:-words}
    COMPREPLY=()
    if [[ $mode == files ]]; then
        prefix=${reply[1]:-}
        if [[ -n $prefix && $cur == "$prefix"* ]]; then
            cur=${cur#"$prefix"}
        else
            prefix=""
        fi
        while IFS= read -r item; do COMPREPLY+=("$prefix$item"); done < <(compgen -f -- "$cur")
        compopt -o filenames 2>/dev/null || true
    else
        for item in "${reply[@]:1}"; do
            # Readline replaces only the portion after its last word break.
            if [[ $item == *=* && $cur != *=* ]]; then item=${item#*=}; fi
            COMPREPLY+=("$item")
        done
    fi
}
complete -F _mcpp_complete mcpp mcpp.exe
)MCPP";
    if (shell == "zsh") return R"MCPP(# mcpp completion; source this file after compinit in ~/.zshrc.
if (( ! $+functions[compdef] )); then
    autoload -Uz compinit
    compinit
fi
_mcpp_complete() {
    local -a reply
    reply=("${(@f)$(command "$words[1]" __complete "${words[@]:1:$((CURRENT-1))}" 2>/dev/null)}")
    if [[ $reply[1] == files ]]; then
        if [[ -n $reply[2] ]]; then compset -P "${(b)reply[2]}"; fi
        _files
    else
        compadd -- "${reply[@]:1}"
    fi
}
compdef _mcpp_complete mcpp mcpp.exe
)MCPP";
    if (shell == "fish") return R"MCPP(# mcpp completion; source this file from config.fish.
function __mcpp_candidates
    # --tokenize (-o) exists in fish 3.x; --tokens-expanded (-x) is 4.0+.
    set -l words (commandline -opc)
    set -l current (commandline -ct)
    set -l reply (command $words[1] __complete $words[2..-1] "__mcpp_word__$current" 2>/dev/null)
    if test "$reply[1]" = files
        set -l prefix "$reply[2]"
        if test -n "$prefix"
            set current (string sub -s (math (string length -- "$prefix") + 1) -- "$current")
        end
        for path in (__fish_complete_path "$current")
            printf '%s%s\n' "$prefix" "$path"
        end
    else
        printf '%s\n' $reply[2..-1]
    end
end
complete -c mcpp -e
complete -c mcpp.exe -e
complete -c mcpp -f -a '(__mcpp_candidates)'
complete -c mcpp.exe -f -a '(__mcpp_candidates)'
)MCPP";
    if (shell == "pwsh") return R"MCPP(# mcpp completion; dot-source this file from $PROFILE.
Register-ArgumentCompleter -Native -CommandName mcpp,mcpp.exe -ScriptBlock {
    param($wordToComplete, $commandAst, $cursorPosition)
    $tokens = @()
    $elements = @($commandAst.CommandElements)
    foreach ($element in $elements | Select-Object -Skip 1) {
        if ($element.Extent.EndOffset -ge $cursorPosition) { break }
        if ($element -is [System.Management.Automation.Language.StringConstantExpressionAst]) {
            $tokens += $element.Value
        } else {
            $tokens += $element.Extent.Text
        }
    }
    $reply = @(& $elements[0].Value __complete @tokens "__mcpp_word__$wordToComplete" 2>$null)
    if ($reply.Count -eq 0) { return }
    if ($reply[0] -eq 'files') {
        $prefix = if ($reply.Count -gt 1) { $reply[1] } else { '' }
        $path = $wordToComplete.Substring($prefix.Length)
        foreach ($match in [System.Management.Automation.CompletionCompleters]::CompleteFilename($path)) {
            [System.Management.Automation.CompletionResult]::new(
                $prefix + $match.CompletionText, $match.ListItemText, $match.ResultType, $match.ToolTip)
        }
        return
    }
    foreach ($candidate in $reply | Select-Object -Skip 1) {
        [System.Management.Automation.CompletionResult]::new($candidate, $candidate, 'ParameterValue', $candidate)
    }
}
)MCPP";
    return {};
}

int install(const std::filesystem::path& home, std::string_view shell = {}) {
    if (!shell.empty() && script(shell).empty()) {
        std::println(stderr, "error: unsupported shell '{}'; expected bash, zsh, pwsh or fish", shell);
        return 2;
    }
    const auto directory = home / "config" / "shell";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        std::println(stderr, "error: cannot create shell config directory: {}", ec.message());
        return 1;
    }
    for (const auto* name : {"bash", "zsh", "pwsh", "fish"}) {
        if (!shell.empty() && shell != name) continue;
        const auto path = directory / (std::string("mcpp.") + (std::string_view(name) == "pwsh" ? "ps1" : name));
        {
            std::ifstream in(path, std::ios::binary);
            std::string existing(std::istreambuf_iterator<char>{in}, {});
            if (existing == script(name)) continue;
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << script(name);
        out.close();
        if (!out) {
            std::println(stderr, "error: cannot write shell completion script");
            return 1;
        }
    }
    return 0;
}

} // namespace mcpp::shell
