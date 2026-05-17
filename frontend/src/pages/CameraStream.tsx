import { useState, useEffect, useRef } from "react";
import { useNavigate } from "react-router";
import { io, type Socket } from "socket.io-client";
import { Button } from "../components/button";
import { Card, CardContent, CardHeader, CardTitle } from "../components/card";
import { Badge } from "../components/badge";
import { Switch } from "../components/switch";
import { Label } from "../components/label";
import {
  Camera,
  PawPrint,
  LogOut,
  Video,
  VideoOff,
  Maximize2,
  Minimize2,
  Bell,
  Settings,
  ArrowUp,
  ArrowDown,
  ArrowLeft,
  ArrowRight,
} from "lucide-react";
import { toast } from "sonner";

type CurrentUser = {
  name: string;
  username: string;
};

type MotionSignal = {
  detected?: boolean;
  detected_at?: string | null;
  source?: string | null;
};

type CameraFramePayload = {
  image?: string;
  captured_at?: string;
};

export function CameraStream() {
  const navigate = useNavigate();
  const [currentUser, setCurrentUser] = useState<CurrentUser | null>(null);
  const [isStreaming, setIsStreaming] = useState(true);
  const [motionDetection, setMotionDetection] = useState(true);
  const [autoTracking, setAutoTracking] = useState(false);
  const [isFullscreen, setIsFullscreen] = useState(false);
  const [isTriggeringBuzzer, setIsTriggeringBuzzer] = useState(false);
  const [isSocketConnected, setIsSocketConnected] = useState(false);
  const [latestFrame, setLatestFrame] = useState<string | null>(null);
  const [latestFrameAt, setLatestFrameAt] = useState<string | null>(null);
  const [activity, setActivity] = useState<string[]>([]);
  const [, setLastMotionSignalAt] = useState<string | null>(null);
  const streamContainerRef = useRef<HTMLDivElement | null>(null);
  const socketRef = useRef<Socket | null>(null);
  const streamSubscribedRef = useRef(false);
  const motionDetectionRef = useRef(motionDetection);

  useEffect(() => {
    motionDetectionRef.current = motionDetection;
  }, [motionDetection]);

  // Fetches and sets the currently authenticated user on component mount
  useEffect(() => {
    let isMounted = true;

    const loadCurrentUser = async () => {
      try {
        const response = await fetch("/api/me", {
          credentials: "include",
        });

        if (!response.ok) {
          throw new Error("Not authenticated");
        }

        const data = (await response.json()) as { user?: CurrentUser };
        if (!data.user) {
          throw new Error("Not authenticated");
        }

        if (isMounted) {
          // Store user session state both in React state and localStorage
          setCurrentUser(data.user);
          localStorage.setItem("petcam_current_user", JSON.stringify(data.user));
        }
      } catch {
        // Clear session and redirect to login if authentication fails
        localStorage.removeItem("petcam_current_user");
        toast.error("Please login first!");
        navigate("/");
      }
    };

    loadCurrentUser();

    return () => {
      isMounted = false; // Cleanup flag to prevent setting state on unmounted component
    };
  }, [navigate]);

  // Initializes and manages the Socket.IO connection for real-time events
  useEffect(() => {
    const socket = io({
      withCredentials: true,
      path: "/socket.io",
      transports: ["polling"], // Use polling to avoid issues with some restrictive networks
      upgrade: false,
    });
    socketRef.current = socket;

    const onConnect = () => {
      setIsSocketConnected(true);
    };

    const onDisconnect = () => {
      setIsSocketConnected(false);
      streamSubscribedRef.current = false;
    };

    // Handles incoming motion detection signals from the backend
    const onMotionSignal = (data: MotionSignal) => {
      // Ignore if motion detection is toggled off or data is invalid
      if (!motionDetectionRef.current || !data.detected || !data.detected_at) {
        return;
      }

      setLastMotionSignalAt((prev) => {
        // Avoid duplicate activity logs for the same timestamp
        if (prev === data.detected_at) {
          return prev;
        }

        setActivity((current) => {
          // Append new activity log and keep only the latest 5 entries
          const newActivity = [
            ...current,
            `${new Date().toLocaleTimeString()} - Movement detected`,
          ];
          return newActivity.slice(-5);
        });
        return data.detected_at ?? prev;
      });
    };

    // Receives and updates the latest frame from the live camera stream
    const onCameraFrame = (data: CameraFramePayload) => {
      if (!data.image) {
        return;
      }

      setLatestFrame(data.image);
      setLatestFrameAt(data.captured_at ?? new Date().toISOString());
    };

    const onCameraStreamError = (data: { message?: string }) => {
      toast.error(data.message || "Camera stream error on backend.");
    };

    const onAutotrackingState = (data: { enabled: boolean }) => {
      setAutoTracking(data.enabled);
      if (!data.enabled) {
        toast.success("Autotracking scan completed!");
      }
    };

    socket.on("connect", onConnect);
    socket.on("disconnect", onDisconnect);
    socket.on("motion_signal", onMotionSignal);
    socket.on("camera_frame", onCameraFrame);
    socket.on("camera_stream_error", onCameraStreamError);
    socket.on("autotracking_state", onAutotrackingState);

    return () => {
      if (socket.connected && streamSubscribedRef.current) {
        socket.emit("unsubscribe_camera_stream");
        streamSubscribedRef.current = false;
      }
      socket.off("connect", onConnect);
      socket.off("disconnect", onDisconnect);
      socket.off("motion_signal", onMotionSignal);
      socket.off("camera_frame", onCameraFrame);
      socket.off("camera_stream_error", onCameraStreamError);
      socket.off("autotracking_state", onAutotrackingState);
      socket.disconnect();
      socketRef.current = null;
      setIsSocketConnected(false);
    };
  }, []);

  // Manages subscribing and unsubscribing to the camera stream based on component state
  useEffect(() => {
    const socket = socketRef.current;
    if (!socket || !socket.connected) {
      return;
    }

    // Subscribe to stream if user has resumed it and we are not currently subscribed
    if (isStreaming && !streamSubscribedRef.current) {
      socket.emit(
        "subscribe_camera_stream",
        { interval_ms: 66 }, // Requesting roughly ~15 FPS
        (response?: { ok?: boolean; message?: string }) => {
          if (!response?.ok) {
            toast.error(response?.message || "Could not subscribe to camera stream.");
            return;
          }

          streamSubscribedRef.current = true;
        }
      );
      return;
    }

    // Unsubscribe from stream if user paused it and we are currently subscribed
    if (!isStreaming && streamSubscribedRef.current) {
      socket.emit("unsubscribe_camera_stream", (response?: { ok?: boolean; message?: string }) => {
        if (!response?.ok) {
          toast.error(response?.message || "Could not pause backend stream.");
          return;
        }

        streamSubscribedRef.current = false;
      });
    }
  }, [isSocketConnected, isStreaming]);

  // Utility function to emit socket events with a timeout and promise the acknowledgment
  const emitSocketEvent = <TResponse,>(event: string, payload?: unknown): Promise<TResponse> => {
    const socket = socketRef.current;
    if (!socket || !socket.connected) {
      return Promise.reject(new Error("Socket not connected"));
    }

    return new Promise<TResponse>((resolve, reject) => {
      // Set a 2000ms timeout for the socket response
      socket.timeout(2000).emit(event, payload, (error: unknown, response: TResponse) => {
        if (error) {
          reject(error);
          return;
        }
        resolve(response);
      });
    });
  };

  useEffect(() => {
    const onFullscreenChange = () => {
      setIsFullscreen(document.fullscreenElement === streamContainerRef.current);
    };

    document.addEventListener("fullscreenchange", onFullscreenChange);
    return () => {
      document.removeEventListener("fullscreenchange", onFullscreenChange);
    };
  }, []);

  const handleLogout = async () => {
    try {
      await fetch("/api/logout", {
        method: "POST",
        credentials: "include",
      });
    } catch {
      // Best effort logout on backend.
    } finally {
      localStorage.removeItem("petcam_current_user");
      toast.success("Logged out successfully!");
      navigate("/");
    }
  };

  const toggleStream = () => {
    setIsStreaming(!isStreaming);
    toast.info(isStreaming ? "Stream paused" : "Stream resumed");
  };

  const handleFullscreen = async () => {
    try {
      if (!document.fullscreenElement && streamContainerRef.current) {
        await streamContainerRef.current.requestFullscreen();
      } else if (document.fullscreenElement) {
        await document.exitFullscreen();
      }
    } catch {
      toast.error("Could not toggle fullscreen on this browser.");
    }
  };

  // Handles manual camera movement and falls back to HTTP if Socket.IO is unavailable
  const handleCameraMove = async (direction: "up" | "down" | "left" | "right") => {
    // Automatically disable auto-tracking when the user manually moves the camera
    if (autoTracking) {
      setAutoTracking(false);
      toast.info("Auto-tracking disabled by manual override");
      try {
        await fetch("/api/settings/autotracking", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ enabled: false }),
        });
      } catch {
        toast.error("Could not sync auto-tracking settings with backend.");
      }
    }

    try {
      // Prefer Socket.IO for lower latency camera control if connected
      if (isSocketConnected) {
        const socketResponse = await emitSocketEvent<{ ok?: boolean; message?: string }>("camera_move", {
          direction,
        });
        if (!socketResponse?.ok) {
          toast.error(socketResponse?.message || "Could not send camera command.");
          return;
        }

        toast.info(`Move camera ${direction}`);
        return;
      }

      // Fallback to REST API if socket connection is down
      const response = await fetch("/api/camera/move", {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
        },
        credentials: "include",
        body: JSON.stringify({ direction }),
      });

      const data = (await response.json()) as { message?: string };
      if (!response.ok) {
        toast.error(data.message || "Could not send camera command.");
        return;
      }

      toast.info(`Move camera ${direction}`);
    } catch {
      toast.error("Could not reach backend camera controls endpoint.");
    }
  };

  // Triggers the buzzer via the backend
  const handleBuzzerTrigger = async () => {
    if (isTriggeringBuzzer) {
      return; // Prevent spamming the buzzer trigger
    }

    setIsTriggeringBuzzer(true);
    try {
      // Prefer real-time socket communication if available
      if (isSocketConnected) {
        const socketResponse = await emitSocketEvent<{ ok?: boolean; message?: string }>("trigger_buzzer");
        if (!socketResponse?.ok) {
          toast.error(socketResponse?.message || "Could not trigger buzzer.");
          return;
        }

        toast.success("Buzzer signal");
        return;
      }

      // Fallback to standard HTTP request
      const response = await fetch("/api/buzzer", {
        method: "POST",
        credentials: "include",
      });

      const data = (await response.json()) as { message?: string };
      if (!response.ok) {
        toast.error(data.message || "Could not trigger buzzer.");
        return;
      }

      toast.success("Buzzer signal");
    } catch {
      toast.error("Could not reach backend buzzer endpoint.");
    } finally {
      setIsTriggeringBuzzer(false);
    }
  };

  const controlButtonClass = "cursor-pointer border-zinc-900 bg-transparent text-zinc-900 hover:bg-black/5";

  return (
    <div className="min-h-screen bg-gradient-to-br from-pink-50 via-purple-50 to-blue-50">
      {/* Header */}
      <header className="bg-white/80 backdrop-blur-sm border-b-2 border-purple-200 shadow-sm">
        <div className="max-w-7xl mx-auto px-4 py-4 flex items-center justify-between">
          <div className="flex items-center gap-3">
            <div className="size-10 bg-gradient-to-br from-pink-400 to-purple-500 rounded-full flex items-center justify-center">
              <Camera className="size-6 text-white" />
            </div>
            <div>
              <h1 className="font-bold bg-gradient-to-r from-pink-500 to-purple-500 bg-clip-text text-transparent">
                PetCam
              </h1>
              <p className="text-sm text-gray-600">Welcome, {currentUser?.name}!</p>
            </div>
          </div>
          <Button
            onClick={handleLogout}
            variant="outline"
            className="border-pink-300 hover:bg-pink-50"
          >
            <LogOut className="size-4 mr-2" />
            Logout
          </Button>
        </div>
      </header>

      <div className="max-w-7xl mx-auto px-4 py-8">
        <div className="grid grid-cols-1 lg:grid-cols-3 gap-6">
          {/* Main Camera Stream */}
          <div className="lg:col-span-2 space-y-4">
            <Card className="border-2 border-purple-200 shadow-xl overflow-hidden gap-0">
              <CardHeader className="bg-gradient-to-r from-pink-100 to-purple-100 pb-4">
                <div className="flex items-center justify-between">
                  <CardTitle className="flex items-center gap-2">
                    <Video className="size-5 text-pink-500" />
                    Felix
                  </CardTitle>
                  <Badge className="bg-green-500 text-white animate-pulse">
                    <span className="size-2 bg-white rounded-full mr-2 inline-block" />
                    LIVE
                  </Badge>
                </div>
              </CardHeader>
              <CardContent className="p-0 [&:last-child]:pb-0">
                <div ref={streamContainerRef} className={`bg-black ${isFullscreen ? "h-screen flex flex-col" : ""}`}>
                  <div className={`relative bg-gray-900 ${isFullscreen ? "flex-1 min-h-0" : "aspect-video"}`}>
                    {isStreaming ? (
                      <>
                        {latestFrame ? (
                          <img
                            src={latestFrame}
                            alt="Camera stream"
                            className="w-full h-full object-cover scale-x-[-1]"
                          />
                        ) : (
                          <div className="w-full h-full flex items-center justify-center">
                            <div className="text-center text-gray-300 px-4">
                              <Video className="size-12 mx-auto mb-3" />
                              <p className="font-medium">Waiting for live camera frames...</p>
                              <p className="text-sm text-gray-400 mt-1">
                                {isSocketConnected
                                  ? "Socket connected. Backend should start sending frames."
                                  : "Socket disconnected. Reconnect in progress..."}
                              </p>
                            </div>
                          </div>
                        )}
                        {/* Simulated timestamp overlay */}
                        <div className="absolute top-4 left-4 bg-black/60 text-white px-3 py-1 rounded text-sm font-mono">
                          {latestFrameAt ? new Date(latestFrameAt).toLocaleTimeString() : new Date().toLocaleTimeString()}
                        </div>
                        {/* Motion detection indicator */}
                        {motionDetection && (
                          <div className="absolute top-4 right-4 bg-red-500/80 text-white px-3 py-1 rounded text-sm flex items-center gap-2 animate-pulse">
                            <Bell className="size-4" />
                            Motion Detection ON
                          </div>
                        )}
                      </>
                    ) : (
                      <div className="w-full h-full flex items-center justify-center">
                        <div className="text-center text-gray-400">
                          <VideoOff className="size-16 mx-auto mb-4" />
                          <p>Stream Paused</p>
                        </div>
                      </div>
                    )}
                  </div>

                  {/* Controls */}
                  <div
                    className={`bg-gradient-to-r from-pink-100 to-purple-100 border-t border-purple-100 ${isFullscreen ? "p-2 shrink-0" : "p-3"
                      }`}
                  >
                    <div className="flex items-center justify-between gap-3">
                      <div className="w-32 flex justify-start">
                        <Button
                          type="button"
                          variant="outline"
                          onClick={toggleStream}
                          size={isFullscreen ? "sm" : "default"}
                          className={controlButtonClass}
                        >
                          {isStreaming ? (
                            <>
                              <Video className="size-4 mr-2" />
                              Pause
                            </>
                          ) : (
                            <>
                              <VideoOff className="size-4 mr-2" />
                              Resume
                            </>
                          )}
                        </Button>
                      </div>

                      <div className="grid grid-cols-3 gap-1.5 w-[132px]">
                        <div />
                        <Button
                          type="button"
                          variant="outline"
                          size="sm"
                          onClick={() => handleCameraMove("up")}
                          aria-label="Move camera up"
                          className={controlButtonClass}
                        >
                          <ArrowUp className="size-4" />
                        </Button>
                        <div />
                        <Button
                          type="button"
                          variant="outline"
                          size="sm"
                          onClick={() => handleCameraMove("left")}
                          aria-label="Move camera left"
                          className={controlButtonClass}
                        >
                          <ArrowLeft className="size-4" />
                        </Button>
                        <div className="h-8" />
                        <Button
                          type="button"
                          variant="outline"
                          size="sm"
                          onClick={() => handleCameraMove("right")}
                          aria-label="Move camera right"
                          className={controlButtonClass}
                        >
                          <ArrowRight className="size-4" />
                        </Button>
                        <div />
                        <Button
                          type="button"
                          variant="outline"
                          size="sm"
                          onClick={() => handleCameraMove("down")}
                          aria-label="Move camera down"
                          className={controlButtonClass}
                        >
                          <ArrowDown className="size-4" />
                        </Button>
                        <div />
                      </div>

                      <div className="w-32 flex justify-end">
                        <Button
                          type="button"
                          onClick={handleFullscreen}
                          variant="outline"
                          size={isFullscreen ? "sm" : "default"}
                          className={controlButtonClass}
                        >
                          {isFullscreen ? <Minimize2 className="size-5" /> : <Maximize2 className="size-5" />}
                        </Button>
                      </div>
                    </div>
                  </div>
                </div>
              </CardContent>
            </Card>
          </div>

          {/* Sidebar */}
          <div className="space-y-4">
            <Button
              type="button"
              onClick={handleBuzzerTrigger}
              disabled={isTriggeringBuzzer}
              className="w-full border-0 bg-gradient-to-r from-pink-500 to-purple-500 text-white shadow-md transition-all hover:from-pink-600 hover:to-purple-600 hover:shadow-lg focus-visible:ring-pink-300 disabled:opacity-70"
            >
              <Bell className="size-4 mr-2" />
              {isTriggeringBuzzer ? "Sending buzzer signal..." : "Make a sound (distract cat)"}
            </Button>

            {/* Settings */}
            <Card className="border-2 border-purple-200">
              <CardHeader className="bg-gradient-to-r from-pink-50 to-purple-50">
                <CardTitle className="flex items-center gap-2">
                  <Settings className="size-5 text-pink-500" />
                  Settings
                </CardTitle>
              </CardHeader>
              <CardContent className="pt-4 space-y-4">
                <div className="flex items-center justify-between">
                  <Label htmlFor="motion-detection" className="flex items-center gap-2 cursor-pointer">
                    <Bell className="size-4 text-purple-500" />
                    Notify me when motion is detected
                  </Label>
                  <Switch
                    id="motion-detection"
                    checked={motionDetection}
                    onCheckedChange={async (checked) => {
                      setMotionDetection(checked);
                      toast.info(
                        checked ? "Motion alerts enabled" : "Motion alerts disabled"
                      );
                      try {
                        await fetch("/api/settings/notifications", {
                          method: "POST",
                          headers: { "Content-Type": "application/json" },
                          body: JSON.stringify({ enabled: checked }),
                        });
                      } catch {
                        toast.error("Could not sync notification settings with backend.");
                      }
                    }}
                  />
                </div>
                <div className="flex items-center justify-between">
                  <Label htmlFor="auto-tracking" className="flex items-center gap-2 cursor-pointer">
                    <Video className="size-4 text-purple-500" />
                    Auto-tracking
                  </Label>
                  <Switch
                    id="auto-tracking"
                    checked={autoTracking}
                    onCheckedChange={async (checked) => {
                      setAutoTracking(checked);
                      toast.info(
                        checked ? "Auto-tracking enabled" : "Auto-tracking disabled"
                      );
                      try {
                        await fetch("/api/settings/autotracking", {
                          method: "POST",
                          headers: { "Content-Type": "application/json" },
                          body: JSON.stringify({ enabled: checked }),
                        });
                      } catch {
                        toast.error("Could not sync auto-tracking settings with backend.");
                      }
                    }}
                  />
                </div>
              </CardContent>
            </Card>

            {/* Activity Log */}
            <Card className="border-2 border-purple-200">
              <CardHeader className="bg-gradient-to-r from-pink-50 to-purple-50">
                <CardTitle className="flex items-center gap-2">
                  <PawPrint className="size-5 text-pink-500" />
                  Recent Activity
                </CardTitle>
              </CardHeader>
              <CardContent className="pt-4">
                {activity.length > 0 ? (
                  <div className="space-y-2">
                    {activity.map((act, idx) => (
                      <div
                        key={idx}
                        className="flex items-center gap-2 p-2 bg-purple-50 rounded text-sm"
                      >
                        {act}
                      </div>
                    ))}
                  </div>
                ) : (
                  <p className="text-gray-500 text-sm">No recent activity detected</p>
                )}
              </CardContent>
            </Card>
          </div>
        </div>
      </div>
    </div>
  );
}
