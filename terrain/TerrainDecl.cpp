#include "TerrainDecl.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace OverboardTerrain
{
	namespace
	{
		std::string Trim(const std::string& S)
		{
			size_t B = S.find_first_not_of(" \t\r\n");
			if (B == std::string::npos)
			{
				return "";
			}
			size_t E = S.find_last_not_of(" \t\r\n");
			return S.substr(B, E - B + 1);
		}

		// Parses a decimal number and REJECTS trailing junk. `strtod` alone would happily read
		// "0.5deg" as 0.5, which is how a unit suffix silently becomes a wrong number.
		bool ParseNumber(const std::string& Tok, double& Out)
		{
			if (Tok.empty())
			{
				return false;
			}
			errno = 0;
			char* End = nullptr;
			const double V = std::strtod(Tok.c_str(), &End);
			if (errno != 0 || End == Tok.c_str() || *End != '\0')
			{
				return false;
			}
			Out = V;
			return true;
		}

		std::string Where(const std::string& Path, int Line)
		{
			return Path + ":" + std::to_string(Line) + ": ";
		}
	}

	bool HashFile(const std::string& Path, uint64_t& OutHash, std::string& OutError)
	{
		std::ifstream F(Path, std::ios::binary);
		if (!F)
		{
			OutError = "cannot open '" + Path + "'";
			return false;
		}

		uint64_t H = 14695981039346656037ull; // FNV-1a 64 offset basis
		char Buf[65536];
		while (F.read(Buf, sizeof(Buf)) || F.gcount() > 0)
		{
			const std::streamsize N = F.gcount();
			for (std::streamsize i = 0; i < N; ++i)
			{
				H ^= static_cast<uint8_t>(Buf[i]);
				H *= 1099511628211ull; // FNV-1a 64 prime
			}
			if (!F)
			{
				break;
			}
		}

		OutHash = H;
		return true;
	}

	bool ParseLevelDecl(const std::string& Path, FLevelDecl& OutDecl, std::string& OutError)
	{
		std::ifstream F(Path);
		if (!F)
		{
			OutError = "cannot open '" + Path + "'";
			return false;
		}

		OutDecl = FLevelDecl{};
		OutDecl.DeclPath = Path;

		FSurfaceDecl* Cur = nullptr;
		bool SawLevel = false;
		bool SawHash = false;
		std::string Line;
		int LineNo = 0;

		while (std::getline(F, Line))
		{
			++LineNo;
			const std::string T = Trim(Line);
			if (T.empty() || T[0] == '#')
			{
				continue;
			}

			std::istringstream SS(T);
			std::string Key;
			SS >> Key;
			std::string Rest;
			std::getline(SS, Rest);
			Rest = Trim(Rest);

			if (Key == "level")
			{
				if (Rest.empty())
				{
					OutError = Where(Path, LineNo) + "`level` needs a repo-relative .umap path";
					return false;
				}
				OutDecl.LevelPath = Rest;
				SawLevel = true;
			}
			else if (Key == "level_hash")
			{
				errno = 0;
				char* End = nullptr;
				const unsigned long long V = std::strtoull(Rest.c_str(), &End, 16);
				if (errno != 0 || End == Rest.c_str() || *End != '\0')
				{
					OutError = Where(Path, LineNo) + "`level_hash` must be a 64-bit hex value";
					return false;
				}
				OutDecl.LevelHash = static_cast<uint64_t>(V);
				SawHash = true;
			}
			else if (Key == "note")
			{
				// Level-scope prose that the CHECK PRINTS on every run. A `#` comment would say
				// the same thing to a reader who opens the file; a note says it to everyone who
				// reads a CI log. Used for divergences the envelope deliberately does not cover,
				// which are exactly the things that go quiet if nobody is made to look at them.
				if (Rest.empty())
				{
					OutError = Where(Path, LineNo) + "`note` with no text";
					return false;
				}
				OutDecl.Notes.push_back(Rest);
			}
			else if (Key == "surface")
			{
				if (Rest.empty())
				{
					OutError = Where(Path, LineNo) + "`surface` needs a name";
					return false;
				}
				OutDecl.Surfaces.push_back(FSurfaceDecl{});
				Cur = &OutDecl.Surfaces.back();
				Cur->Name = Rest;
			}
			else if (Cur == nullptr)
			{
				OutError = Where(Path, LineNo) + "`" + Key + "` appears before any `surface`";
				return false;
			}
			else if (Key == "kind")
			{
				if (Rest == "plane") { Cur->Kind = ESurfaceKind::Plane; }
				else if (Rest == "mesh") { Cur->Kind = ESurfaceKind::Mesh; }
				else if (Rest == "probe") { Cur->Kind = ESurfaceKind::Probe; }
				else if (Rest == "external") { Cur->Kind = ESurfaceKind::External; }
				else
				{
					OutError = Where(Path, LineNo) + "`kind` must be plane|mesh|probe|external, got '" + Rest + "'";
					return false;
				}
			}
			else if (Key == "status")
			{
				if (Rest == "verified") { Cur->Status = EVerifyStatus::Verified; }
				else if (Rest == "unverified") { Cur->Status = EVerifyStatus::Unverified; }
				else
				{
					OutError = Where(Path, LineNo) + "`status` must be verified|unverified, got '" + Rest + "'";
					return false;
				}
			}
			else if (Key == "source")
			{
				Cur->Source = Rest;
			}
			else if (Key == "slope_deg")
			{
				if (!ParseNumber(Rest, Cur->SlopeDeg))
				{
					OutError = Where(Path, LineNo) + "`slope_deg` is not a plain number: '" + Rest + "'";
					return false;
				}
			}
			else if (Key == "run_out_m")
			{
				if (!ParseNumber(Rest, Cur->RunOutM))
				{
					OutError = Where(Path, LineNo) + "`run_out_m` is not a plain number: '" + Rest + "'";
					return false;
				}
			}
			else if (Key == "reach_m")
			{
				if (!ParseNumber(Rest, Cur->ReachM))
				{
					OutError = Where(Path, LineNo) + "`reach_m` is not a plain number: '" + Rest + "'";
					return false;
				}
			}
			else if (Key == "path")
			{
				Cur->Path = Rest;
			}
			else if (Key == "units")
			{
				if (Rest == "mm") { Cur->UnitsToMm = 1.0; }
				else if (Rest == "cm") { Cur->UnitsToMm = 10.0; }
				else if (Rest == "m") { Cur->UnitsToMm = 1000.0; }
				else
				{
					OutError = Where(Path, LineNo) + "`units` must be mm|cm|m, got '" + Rest + "'";
					return false;
				}
			}
			else if (Key == "probe")
			{
				std::istringstream PS(Rest);
				std::string A, B, C;
				PS >> A >> B >> C;
				FProbePoint P;
				if (!ParseNumber(A, P.XM) || !ParseNumber(B, P.YM) || !ParseNumber(C, P.ZM))
				{
					OutError = Where(Path, LineNo) + "`probe` needs three numbers: x_m y_m z_m";
					return false;
				}
				Cur->Probes.push_back(P);
			}
			else
			{
				// Strict on purpose. A typo'd key that parsed as a comment would read as
				// coverage this file does not have.
				OutError = Where(Path, LineNo) + "unknown key `" + Key + "`";
				return false;
			}
		}

		if (!SawLevel)
		{
			OutError = Path + ": no `level` line";
			return false;
		}
		if (!SawHash)
		{
			OutError = Path + ": no `level_hash` line -- without it this declaration cannot go stale loudly";
			return false;
		}
		if (OutDecl.Surfaces.empty())
		{
			OutError = Path + ": declares no surfaces. A level the board cannot be driven on is not a thing this project ships; say so with an explicit surface if it really has none.";
			return false;
		}

		for (const FSurfaceDecl& S : OutDecl.Surfaces)
		{
			if (S.Source.empty())
			{
				OutError = Path + ": surface '" + S.Name + "' has no `source`. Every number here needs to say where it came from.";
				return false;
			}
			if (S.Kind == ESurfaceKind::Mesh && S.Path.empty())
			{
				OutError = Path + ": surface '" + S.Name + "' is kind=mesh but has no `path`";
				return false;
			}
			if (S.Kind == ESurfaceKind::External)
			{
				if (S.Status == EVerifyStatus::Verified)
				{
					OutError = Path + ": surface '" + S.Name + "' is kind=external and status=verified. Geometry this repo cannot open has not been verified by anyone; measure it with the terrain probe and declare it as kind=probe instead.";
					return false;
				}
				if (S.ReachM <= 0.0)
				{
					OutError = Path + ": surface '" + S.Name + "' is kind=external but declares no `reach_m`. If the ground is unmeasured, the declaration must at least state HOW MUCH of it is unmeasured.";
					return false;
				}
			}
			if (S.Kind == ESurfaceKind::Probe)
			{
				if (S.Probes.size() < 2)
				{
					OutError = Path + ": surface '" + S.Name + "' is kind=probe with fewer than two `probe` points. One point measures a height, not a surface.";
					return false;
				}
				if (S.ReachM <= 0.0)
				{
					OutError = Path + ": surface '" + S.Name + "' is kind=probe but declares no `reach_m`. Probe coverage is meaningless without the distance it has to cover.";
					return false;
				}
			}
		}

		return true;
	}
}
