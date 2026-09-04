#include "broker/UploadQueue.hpp"
#include "logging/Logger.hpp"
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <format>

namespace evidence {
namespace {
std::int64_t unix_now() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
nlohmann::json metadata_json(const EvidenceMetadata& m) { return {{"admin",m.admin},{"target_id",m.target_id},{"target_name",m.target_name},{"server",m.server},{"timestamp",m.timestamp},{"punishment_command",m.punishment_command},{"punishment_reason",m.punishment_reason},{"recording_period",m.recording_period}}; }
EvidenceMetadata metadata_from(const nlohmann::json& j) { EvidenceMetadata m; m.admin=j.value("admin","");m.target_id=j.value("target_id",-1);m.target_name=j.value("target_name","");m.server=j.value("server","");m.timestamp=j.value("timestamp","");m.punishment_command=j.value("punishment_command","");m.punishment_reason=j.value("punishment_reason","");m.recording_period=j.value("recording_period","");return m; }
}

UploadQueue::UploadQueue(std::filesystem::path state_file, Config::Broker settings)
    : state_file_(std::move(state_file)), settings_(std::move(settings)) {}
UploadQueue::~UploadQueue() { stop(); }
void UploadQueue::start() { load(); if (!thread_.joinable()) thread_ = std::jthread([this](std::stop_token token){ run(token); }); }
void UploadQueue::stop() { if (thread_.joinable()) { thread_.request_stop(); thread_.join(); } }

void UploadQueue::load() {
  std::scoped_lock lock(mutex_); jobs_.clear(); std::ifstream input(state_file_); if (!input) return;
  try { auto root=nlohmann::json::parse(input); for (const auto& j:root.value("jobs",nlohmann::json::array())) jobs_.push_back({j.value("id",""),j.value("path",""),metadata_from(j.value("metadata",nlohmann::json::object())),j.value("attempts",0u),j.value("next_attempt",0ll),j.value("upload_id","")}); }
  catch (...) { log_warn("UPLOAD_QUEUE_INVALID"); }
}
void UploadQueue::save_locked() const {
  std::filesystem::create_directories(state_file_.parent_path()); nlohmann::json jobs=nlohmann::json::array();
  for(const auto& j:jobs_) jobs.push_back({{"id",j.id},{"path",j.path.string()},{"metadata",metadata_json(j.metadata)},{"attempts",j.attempts},{"next_attempt",j.next_attempt},{"upload_id",j.upload_id}});
  const auto temporary=state_file_.wstring()+L".tmp"; { std::ofstream out(temporary,std::ios::trunc); out<<nlohmann::json{{"version",1},{"jobs",jobs}}.dump(2)<<'\n'; }
  MoveFileExW(temporary.c_str(),state_file_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
}
std::string UploadQueue::enqueue(const std::filesystem::path& path, const EvidenceMetadata& metadata) {
  std::scoped_lock lock(mutex_); const auto id=std::format("{}-{}",GetCurrentProcessId(),std::chrono::steady_clock::now().time_since_epoch().count());
  jobs_.push_back({id,path,metadata,0,unix_now(),{}}); save_locked(); return id;
}
nlohmann::json UploadQueue::status() const {
  std::scoped_lock lock(mutex_); const auto sent=sent_.load(),total=total_.load(); nlohmann::json result{{"in_progress",!active_id_.empty()},{"active_id",active_id_},{"pending",jobs_.size()},{"sent_bytes",sent},{"total_bytes",total},{"percent",total?static_cast<unsigned>(sent*100/total):0}};
  if(last_) {result["last_success"]=last_->success;result["last_url"]=last_->url;result["last_error"]=last_->error;} return result;
}
void UploadQueue::configure(Config::Broker settings) { std::scoped_lock lock(mutex_); settings_ = std::move(settings); }
void UploadQueue::enforce_archive_limit(const std::filesystem::path& directory, std::uint64_t limit_bytes,
                                        const std::filesystem::path& keep) {
  struct File { std::filesystem::path path; std::uint64_t size; std::filesystem::file_time_type time; };
  std::scoped_lock lock(mutex_); std::vector<File> files; std::uint64_t total{}; std::error_code ec;
  const auto root = std::filesystem::weakly_canonical(directory, ec); if (ec) return;
  for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
    if (ec) { ec.clear(); continue; } if (!it->is_regular_file(ec) || it->path().extension() != ".mp4") continue;
    const auto path = std::filesystem::weakly_canonical(it->path(), ec); if (ec) { ec.clear(); continue; }
    const auto size = it->file_size(ec); if (ec) { ec.clear(); continue; } total += size; files.push_back({path,size,it->last_write_time(ec)});
  }
  std::sort(files.begin(),files.end(),[](const File& a,const File& b){return a.time<b.time;});
  for(const auto& file:files) {
    if(total<=limit_bytes) break;
    if((!keep.empty() && std::filesystem::equivalent(file.path,keep,ec)) || std::any_of(jobs_.begin(),jobs_.end(),[&](const Job& job){return std::filesystem::equivalent(file.path,job.path,ec);})) continue;
    if(std::filesystem::remove(file.path,ec)){total-=file.size;log_info("ARCHIVE_EVICTED","path=\""+file.path.string()+"\"");} ec.clear();
  }
}
void UploadQueue::run(std::stop_token token) {
  while(!token.stop_requested()) {
    Job job; Config::Broker settings; bool found=false;
    { std::scoped_lock lock(mutex_); settings=settings_; const auto now=unix_now(); auto it=std::find_if(jobs_.begin(),jobs_.end(),[&](const Job& value){return value.next_attempt<=now;}); if(it!=jobs_.end()){job=*it;active_id_=job.id;sent_=0;total_=0;found=true;} }
    if(!found){for(int i=0;i<10&&!token.stop_requested();++i)Sleep(100);continue;}
    auto result=BrokerUploader(settings).upload(job.path,job.metadata,[this](auto sent,auto total){sent_=sent;total_=total;},job.upload_id,token);
    { std::scoped_lock lock(mutex_); last_=result; active_id_.clear(); auto it=std::find_if(jobs_.begin(),jobs_.end(),[&](const Job& value){return value.id==job.id;}); if(it!=jobs_.end()){ if(result.success){jobs_.erase(it);}else{it->upload_id=result.upload_id;++it->attempts;const auto backoff=std::min<std::int64_t>(3600,static_cast<std::int64_t>(settings.retry_seconds)*(1ll<<std::min(it->attempts,6u)));it->next_attempt=unix_now()+backoff;} save_locked(); }
    }
  }
}
} // namespace evidence
