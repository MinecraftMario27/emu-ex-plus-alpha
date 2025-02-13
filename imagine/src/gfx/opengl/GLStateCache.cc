/*  This file is part of Imagine.

	Imagine is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	Imagine is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Imagine.  If not, see <http://www.gnu.org/licenses/> */

#include <imagine/gfx/opengl/GLStateCache.hh>
#include "utils.hh"
#include <cstring>

bool GLStateCache::verifyState = false;

void GLStateCache::blendFunc(GLenum sfactor, GLenum dfactor)
{
	if(!(sfactor == blendFuncSfactor && dfactor == blendFuncDfactor))
	{
		runGLCheckedVerbose([&]()
		{
			glBlendFunc(sfactor, dfactor);
		}, "glBlendFunc()");
		blendFuncSfactor = sfactor;
		blendFuncDfactor = dfactor;
	}
}

void GLStateCache::blendEquation(GLenum mode)
{
	if(mode != blendEquationState)
	{
		runGLCheckedVerbose([&]()
		{
			glBlendEquation(mode);
		}, "glBlendEquation()");
		blendEquationState = mode;
	}
}

int8_t *GLStateCache::getCap(GLenum cap)
{
	#define GLCAP_CASE(cap) case cap: return &stateCap.cap ## _state
	switch(cap)
	{
		#ifdef CONFIG_GFX_OPENGL_FIXED_FUNCTION_PIPELINE
		GLCAP_CASE(GL_TEXTURE_2D);
		#endif
		GLCAP_CASE(GL_BLEND);
		GLCAP_CASE(GL_SCISSOR_TEST);
	}
	#undef GLCAP_CASE
	return nullptr;
}

void GLStateCache::enable(GLenum cap)
{
	auto state = getCap(cap);
	if(!state) [[unlikely]]
	{
		// unmanaged cap
		logDMsg("glEnable unmanaged %d", (int)cap);
		runGLCheckedVerbose([&]()
		{
			glEnable(cap);
		}, "glEnable()");
		return;
	}

	if(!(*state) || *state == -1)
	{
		// not enabled or unset
		//logMsg("glEnable %d", (int)cap);
		runGLCheckedVerbose([&]()
		{
			glEnable(cap);
		}, "glEnable()");
		*state = 1;
	}

	if(verifyState)
	{
		GLboolean enabled = true;
		if(runGLCheckedVerbose([&]() { enabled = glIsEnabled(cap); })
				&& !enabled)
		{
			bug_unreachable("state %d out of sync", cap);
		}
	}
}

void GLStateCache::disable(GLenum cap)
{
	auto state = getCap(cap);
	if(!state) [[unlikely]]
	{
		// unmanaged cap
		logDMsg("glDisable unmanaged %d", (int)cap);
		runGLCheckedVerbose([&]()
		{
			glDisable(cap);
		}, "glDisable()");
		return;
	}

	if((*state))
	{
		// is enabled or unset
		//logMsg("glDisable %d", (int)cap);
		runGLCheckedVerbose([&]()
		{
			glDisable(cap);
		}, "glDisable()");
		*state = 0;
	}

	if(verifyState)
	{
		GLboolean enabled = false;
		if(runGLCheckedVerbose([&]() { enabled = glIsEnabled(cap); })
				&& enabled)
		{
			bug_unreachable("state %d out of sync", cap);
		}
	}
}

GLboolean GLStateCache::isEnabled(GLenum cap)
{
	auto state = getCap(cap);
	if(!state) [[unlikely]]
	{
		// unmanaged cap
		logDMsg("glIsEnabled unmanaged %d", (int)cap);
		return glIsEnabled(cap);
	}

	return *state && *state != -1;
}

#ifdef CONFIG_GFX_OPENGL_FIXED_FUNCTION_PIPELINE
int8_t *GLStateCache::getClientCap(GLenum cap)
{
	#define GLCAP_CASE(cap) case cap: return &clientStateCap.cap ## _state
	switch(cap)
	{
		GLCAP_CASE(GL_VERTEX_ARRAY);
		GLCAP_CASE(GL_TEXTURE_COORD_ARRAY);
		GLCAP_CASE(GL_COLOR_ARRAY);
	default: return {};
	}
	#undef GLCAP_CASE
}

void GLStateCache::enableClientState(GLenum cap)
{
	auto state = getClientCap(cap);
	if(!state) [[unlikely]]
	{
		// unmanaged cap
		logDMsg("glEnableClientState unmanaged %d", (int)cap);
		runGLCheckedVerbose([&]()
		{
			glEnableClientState(cap);
		}, "glEnableClientState()");
		return;
	}

	if(!(*state) || *state == -1) // not enabled or unset
	{
		//logMsg("glEnableClientState %d", (int)cap);
		runGLCheckedVerbose([&]()
		{
			glEnableClientState(cap);
		}, "glEnableClientState()");
		*state = 1;
	}

	if(verifyState)
	{
		GLboolean enabled = true;
		if(runGLCheckedVerbose([&]() { enabled = glIsEnabled(cap); })
				&& !enabled)
		{
			bug_unreachable("state %d out of sync", cap);
		}
	}
}

void GLStateCache::disableClientState(GLenum cap)
{
	auto state = getClientCap(cap);
	if(!state) [[unlikely]]
	{
		// unmanaged cap
		logDMsg("glDisableClientState unmanaged %d", (int)cap);
		runGLCheckedVerbose([&]()
		{
			glDisableClientState(cap);
		}, "glDisableClientState()");
		return;
	}

	if((*state)) // is enabled or unset
	{
		//logMsg("glDisableClientState %d", (int)cap);
		runGLCheckedVerbose([&]()
		{
			glDisableClientState(cap);
		}, "glDisableClientState()");
		*state = 0;
	}

	if(verifyState)
	{
		GLboolean enabled = false;
		if(runGLCheckedVerbose([&]() { enabled = glIsEnabled(cap); })
				&& enabled)
		{
			bug_unreachable("state %d out of sync", cap);
		}
	}
}

void GLStateCache::color4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
	if(red != colorState[0] || green != colorState[1] || blue != colorState[2] || alpha != colorState[3])
	{
		glColor4f(red, green, blue, alpha);
		colorState[0] = red; colorState[1] = green; colorState[2] = blue; colorState[3] = alpha;
	}
	else
	{
		//logMsg("glColor state cache hit");
	}
}
#endif
