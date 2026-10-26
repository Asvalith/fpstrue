// Copyright Epic Games, Inc. All Rights Reserved.
// 离线读取引擎 NetTrace Provider，保留 packet/bunch 的真实编号和状态；不自行猜测 ACK 或重传。
#include "CoreMinimal.h"
#if WITH_EDITOR
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "TraceServices/ITraceServicesModule.h"
#include "TraceServices/AnalysisService.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Bookmarks.h"
#include "TraceServices/Model/NetProfiler.h"

namespace
{
FString CsvQuote(const TCHAR* Value)
{
	return TEXT("\"") + FString(Value != nullptr ? Value : TEXT("")).Replace(TEXT("\""), TEXT("\"\"")) + TEXT("\"");
}

void ExportNetworkTrace(const TArray<FString>& Args)
{
	if (Args.Num() != 2 || IFileManager::Get().FileSize(*Args[0]) <= 0 || FPaths::FileExists(Args[1]))
	{
		UE_LOG(LogTemp, Error, TEXT("NET_TRACE_EXPORT FAIL Expected=fp.ExportNetworkTrace <nonempty.utrace> <new.csv>"));
		return;
	}
	auto& Module = FModuleManager::LoadModuleChecked<ITraceServicesModule>(TEXT("TraceServices"));
	auto Session = Module.GetAnalysisService()->StartAnalysis(*Args[0]);
	if (!Session.IsValid()) { UE_LOG(LogTemp, Error, TEXT("NET_TRACE_EXPORT FAIL Reason=NoAnalysisSession")); return; }
	const double Deadline = FPlatformTime::Seconds() + 60.0;
	while (!Session->IsAnalysisComplete() && FPlatformTime::Seconds() < Deadline) FPlatformProcess::Sleep(.01f);
	if (!Session->IsAnalysisComplete())
	{
		Session->Stop(true);
		UE_LOG(LogTemp, Error, TEXT("NET_TRACE_EXPORT FAIL Reason=AnalysisTimeout"));
		return;
	}
	TraceServices::FAnalysisSessionReadScope Scope(*Session);
	const auto* Provider = TraceServices::ReadNetProfilerProvider(*Session);
	if (Provider == nullptr || Provider->GetNetTraceVersion() == 0)
	{ UE_LOG(LogTemp, Error, TEXT("NET_TRACE_EXPORT FAIL Reason=NoNativeNetworkEvents")); return; }
	TArray<FString> Rows;
	Rows.Reserve(8192);
	Rows.Add(TEXT("GameInstance,Connection,Direction,PacketSequence,PacketTime,PacketBytes,DeliveryStatus,EventName,StartBit,EndBit,Level,Channel,BunchSequence,Reliable,ObjectNetId"));
	uint32 Packets = 0, Events = 0;
	bool bOverflow = false;
	Provider->ReadGameInstances([&](const TraceServices::FNetProfilerGameInstance& Game)
	{
		Provider->ReadConnections(Game.GameInstanceIndex, [&](const TraceServices::FNetProfilerConnection& Connection)
		{
			for (uint8 Direction = 0; Direction < TraceServices::ENetProfilerConnectionMode::Count; ++Direction)
			{
				const auto Mode = static_cast<TraceServices::ENetProfilerConnectionMode>(Direction);
				const uint32 Count = Provider->GetPacketCount(Connection.ConnectionIndex, Mode);
				if (Count == 0) continue;
				Provider->EnumeratePackets(Connection.ConnectionIndex, Mode, 0, Count - 1, [&](const TraceServices::FNetProfilerPacket& Packet)
				{
					if (Rows.Num() >= 2000000) { bOverflow = true; return; }
					++Packets;
					const FString Prefix = FString::Printf(TEXT("%u,%u,%s,%u,%.9f,%u,%u,"), Game.GameInstanceIndex, Connection.ConnectionId,
						Direction == TraceServices::Outgoing ? TEXT("Outgoing") : TEXT("Incoming"), Packet.SequenceNumber,
						Packet.TimeStamp, Packet.TotalPacketSizeInBytes, static_cast<uint8>(Packet.DeliveryStatus));
					Rows.Add(Prefix + TEXT("\"Packet\",0,0,0,0,0,0,0"));
					if (Packet.EventCount == 0) return;
					Provider->EnumeratePacketContentEventsByIndex(Connection.ConnectionIndex, Mode,
						Packet.StartEventIndex, Packet.StartEventIndex + Packet.EventCount - 1,
						[&](const TraceServices::FNetProfilerContentEvent& Event)
						{
							if (Rows.Num() >= 2000000) { bOverflow = true; return; }
							FString Name;
							Provider->ReadName(Event.NameIndex, [&](const TraceServices::FNetProfilerName& Entry) { Name = Entry.Name; });
							if (!Event.BunchInfo.bIsValid && !Name.Contains(TEXT("Shot")) && !Name.Contains(TEXT("TargetData"))
								&& !Name.Contains(TEXT("ActivateAbility")) && !Name.Contains(TEXT("EndAbility")) && !Name.Contains(TEXT("ReplicatedEvent"))) return;
							uint64 ObjectId = 0;
							if (Event.ObjectInstanceIndex != 0)
								Provider->ReadObject(Game.GameInstanceIndex, Event.ObjectInstanceIndex,
									[&](const TraceServices::FNetProfilerObjectInstance& Object) { ObjectId = Object.NetObjectId; });
							Rows.Add(Prefix + CsvQuote(*Name) + FString::Printf(TEXT(",%u,%u,%u,%u,%u,%u,%llu"),
								uint32(Event.StartPos), uint32(Event.EndPos), uint32(Event.Level), uint32(Event.BunchInfo.ChannelIndex),
								uint32(Event.BunchInfo.Seq), uint32(Event.BunchInfo.bReliable), ObjectId));
							++Events;
							});
					});
			}
		});
	});
	if (bOverflow || Packets == 0)
	{ UE_LOG(LogTemp, Error, TEXT("NET_TRACE_EXPORT FAIL Reason=%s"), bOverflow ? TEXT("BoundedRowsExceeded") : TEXT("NoPackets")); return; }
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Args[1]), true);
	if (!FFileHelper::SaveStringArrayToFile(Rows, *Args[1], FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{ UE_LOG(LogTemp, Error, TEXT("NET_TRACE_EXPORT FAIL Reason=WriteFailed")); return; }
	TArray<FString> Bookmarks;
	Bookmarks.Add(TEXT("Time,Text"));
	TraceServices::ReadBookmarkProvider(*Session).EnumerateBookmarks(0.0, Session->GetDurationSeconds(),
		[&](const TraceServices::FBookmark& Bookmark)
		{ if (FCString::Strstr(Bookmark.Text, TEXT("FPShot")) != nullptr) Bookmarks.Add(FString::Printf(TEXT("%.9f,"), Bookmark.Time) + CsvQuote(Bookmark.Text)); });
	if (!FFileHelper::SaveStringArrayToFile(Bookmarks, *(Args[1] + TEXT(".bookmarks.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{ UE_LOG(LogTemp, Error, TEXT("NET_TRACE_EXPORT FAIL Reason=BookmarkWriteFailed")); return; }
	UE_LOG(LogTemp, Display, TEXT("NET_TRACE_EXPORT PASS Packets=%u Events=%u Rows=%d File=%s"), Packets, Events, Rows.Num(), *Args[1]);
}

FAutoConsoleCommand ExportNetworkTraceCommand(TEXT("fp.ExportNetworkTrace"),
	TEXT("Offline native packet/bunch export: <input.utrace> <new.csv>"), FConsoleCommandWithArgsDelegate::CreateStatic(ExportNetworkTrace));
}
#endif
