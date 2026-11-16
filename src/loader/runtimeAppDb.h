#pragma once
namespace Loader {
struct Program;
// One source-bound database lifecycle: AppDb, DbPreparation and DbRecovery.
// Installation does not publish endpoints or mark native preparation complete.
bool InstallSourceBoundAppDb(Program* core);
bool RetireSourceBoundAppDb();
}
