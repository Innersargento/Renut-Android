#pragma once
// GPU backend subsystem Pipelines. See backend.h.
#include "types.h"

namespace renut::gpu {

class GpuBackend;

struct Pipelines {
  explicit Pipelines(GpuBackend& owner) : backend(owner) {}
  GpuBackend& backend;

  std::unique_ptr<ShaderStore> store_;
  std::unordered_map<uint64_t, std::unique_ptr<RenderPipeline>> pipelines_;
  std::mutex pipeline_mutex_;
  std::condition_variable pipeline_ready_;
  // Jobs that only need a look in the driver's cache go first: each takes
  // about a millisecond, and behind a compile it would wait fifty. Then the
  // compiles whose draw has nothing to stand in for it, then the rest.
  std::deque<std::shared_ptr<PipelineJob>> pipeline_lookup_queue_;
  std::deque<std::shared_ptr<PipelineJob>> pipeline_urgent_queue_;
  std::deque<std::shared_ptr<PipelineJob>> pipeline_queue_;
  // The programs workers are compiling, each with the jobs of the same program
  // that wait to find it in the cache instead of compiling it again: several
  // pipelines of one program are usually asked for in the same frame.
  // Under pipeline_mutex_.
  std::unordered_map<uint64_t, std::vector<std::shared_ptr<PipelineJob>>> compiling_programs_;
  // The urgent job a worker takes next: one whose draw was missed in the last
  // few frames before one that has left the screen, then the one missed most.
  // A scene's first frames queue hundreds at once, and in order the walls and
  // floors waited behind every effect queued before them. Under pipeline_mutex_.
  std::shared_ptr<PipelineJob> TakeUrgentJob();
  // Programs with a job queued or running, for this thread: the cache cannot
  // have those yet, so a lookup at the draw would be wasted.
  std::unordered_map<uint64_t, uint32_t> pending_programs_;
  bool lookup_on_workers_ = false;
  static uint64_t ProgramKey(const RenderGraphicsPipelineDesc& desc);
  // Takes a finished job's numbers into the stats and out of pending_programs_.
  void CountJob(const PipelineJob& job);
  std::vector<std::thread> pipeline_workers_;
  bool stop_pipeline_worker_ = false;
  void StartPipelineWorker();
  void StopPipelineWorker();
  // Moves every finished job into pipelines_, whether or not its draw came back.
  void ReapPipelines();
  // Pipeline recipes (see Recipe): whether they are kept, the keys already in
  // the file and the file being appended to; the pipelines the precompilation
  // built, each taken by the first draw that asks for it; and its threads.
  bool recipes_ = false;
  std::unordered_set<uint64_t> recorded_recipes_;
  FILE* recipe_file_ = nullptr;
  std::mutex prebuilt_mutex_;
  std::unordered_map<uint64_t, std::unique_ptr<RenderPipeline>> prebuilt_;
  std::vector<std::thread> precompile_threads_;
  std::atomic<bool> stop_precompile_{false};
  void StartRecipes();
  void StopRecipes();
  // Pipeline prediction (PredictPipelines): the fixed state predicted
  // programs are built with, from the first real draw with a pixel shader
  // [0] and the first without [1]; and counts for the log.
  RenderGraphicsPipelineDesc predict_desc_[2];
  bool predict_desc_set_[2] = {};
  uint64_t predict_added_ = 0, predict_known_ = 0, predict_unknown_ = 0, predict_exact_ = 0, predict_raised_ = 0;
  std::unordered_set<uint64_t> drawn_programs_;
  // Turns the queued shader pairs into recipe groups, a few milliseconds at a
  // time; false when it could not make any (no draw yet to borrow a state from).
  bool PredictPipelines();
  // The render states each engine pass (1-8) draws model records with
  // (PassState), as many as it has views; read from the file at start and
  // learned from the draws stamped with their pass (IssueDraw, at a new
  // pipeline). The prediction builds each record of a pass with the most used
  // few of its pass, as real pipelines the draws take as they take the listed
  // ones. Command thread, but for the read at start.
  std::array<std::vector<PassState>, 9> pass_states_;
  void LearnPassState(uint8_t pass, const Recipe& recipe);
  // Recipes the prediction made whole, so a model loaded again adds none.
  std::unordered_set<uint64_t> predicted_recipes_;
  // The pipeline built ahead for this recipe, if there is one; a recipe that
  // is not in the file yet is written to it. `key` is the draw's pipeline key.
  std::unique_ptr<RenderPipeline> TakeRecipe(Recipe& recipe, uint64_t key);
  // Scene tags: which scenes each recipe is drawn in, so a scene load waits
  // for its own pipelines only (PlumeSceneRequested). In the file, a hash of
  // recipe and scene per tag; this run's recipe of each draw key; and the
  // draw keys already seen in the current scene, by their low bits (a
  // collision only misses a tag), so the check per draw is one bit.
  std::unordered_set<uint64_t> scene_tags_;
  std::unordered_map<uint64_t, uint64_t> recipe_of_;
  uint32_t tag_scene_ = 0;
  std::bitset<65536> scene_seen_;
  void NoteSceneUse(uint64_t key);
  void WriteRecord(uint32_t size, const void* data);
};

}  // namespace renut::gpu
