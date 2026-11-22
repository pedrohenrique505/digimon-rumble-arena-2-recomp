// Evita que o rastreador de entrada/saída de funções (PS2_FUNCTION_LOG_TRACKER,
// ligado quando o runtime foi compilado com PS2X_ENABLE_AGRESSIVE_LOGS=ON) faça
// flush() em disco a cada chamada MIPS (~138 MB em 35 s em game/ps2_log.txt).
// Como ps2_log::log_stream/log_entry/log_exit são símbolos fracos (inline) na
// biblioteca gerada, estas definições fortes silenciam o log por padrão sem
// exigir recompilar as ~78k funções geradas. Defina DRA2_ENABLE_FUNC_LOG=1 para
// reativar a gravação em arquivo.
#if defined(AGRESSIVE_LOGS) && AGRESSIVE_LOGS

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ostream>
#include <string>

namespace ps2_log
{
    static bool funcLogEnabled() noexcept
    {
        static const bool enabled = [] {
            const char *v = std::getenv("DRA2_ENABLE_FUNC_LOG");
            return v && *v && *v != '0';
        }();
        return enabled;
    }

    inline int &depth()
    {
        static thread_local int d = 0;
        return d;
    }

    std::ostream &log_stream()
    {
        if (!funcLogEnabled())
        {
            // Mantém depth() em 0 para que loops inline `for (i < depth()) log_stream() << '\t'`
            // em libdra2_generated.a executem 0 iterações (e nunca acumulem se log_entry/exit
            // misturarem chamadas inline e out-of-line).
            depth() = 0;
            static std::ostream nullStream(nullptr);
            return nullStream;
        }
        static std::ofstream f((std::filesystem::current_path() / "ps2_log.txt").string(), std::ios::out);
        return f.is_open() ? static_cast<std::ostream &>(f) : std::cerr;
    }

    void log_entry(const char *name)
    {
        if (!funcLogEnabled())
            return;
        for (int i = 0; i < depth(); ++i)
            log_stream() << '\t';
        log_stream() << ">> " << name << " enter\n";
        log_stream().flush();
        depth()++;
    }

    void log_exit(const char *name)
    {
        if (!funcLogEnabled())
            return;
        depth()--;
        for (int i = 0; i < depth(); ++i)
            log_stream() << '\t';
        log_stream() << "<< " << name << " exit\n";
        log_stream().flush();
    }
} // namespace ps2_log

#endif
